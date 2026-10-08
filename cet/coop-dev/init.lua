-- Cp2077Coop dev panel (Cyber Engine Tweaks). Development only; players never need this.
--
-- * Session controls: host / join / leave / impairment, and live status.
-- * Spike probes (docs/04-feasibility-and-risks.md §3): experiments whose results decide the game-side code.
--   Every probe is wrapped in pcall, so a wrong guess about an engine API prints an error instead of
--   breaking anything, and every risky engine call writes a line to probe-results.txt first, so after a crash
--   the last line names the call. Please send probe-results.txt and the probe-*.txt dumps back.

local panel = {
    overlayOpen = false,
    address = "127.0.0.1:27077",
    password = "",
    port = 27077,
    impairment = 0,
    probeRecord = "Character.TPP_Player_Cutscene_Male",
    itemRecord = "Items.Preset_Lexington_Default",
    itemSlot = "AttachmentSlots.WeaponRight",
    vehicleRecord = "Vehicle.v_standard2_archer_hella_player",
    lines = {},
    pendingUnset = nil,
    lastActivation = 0,
    checks = {},          -- delayed probe checks: { at = os.clock() time, label, fn }
    drive = nil,          -- S1v: a probe vehicle being moved by a teleport every frame
}

local impairments = { "none", "lan", "good", "typical", "bad", "awful" }

-- ---------------------------------------------------------------------------------------------------
-- Logging and small helpers

local function log(text)
    local line = os.date("%H:%M:%S") .. "  " .. tostring(text)
    print("[coop-dev] " .. tostring(text))
    table.insert(panel.lines, line)
    if #panel.lines > 60 then
        table.remove(panel.lines, 1)
    end
    local file = io.open("probe-results.txt", "a")
    if file then
        file:write(line .. "\n")
        file:close()
    end
end

local function try(label, fn)
    local ok, result = pcall(fn)
    if ok then
        log(label .. ": ok" .. (result ~= nil and (" -> " .. tostring(result)) or ""))
    else
        log(label .. ": FAILED -> " .. tostring(result))
    end
    return ok, result
end

local function writeFile(name, content)
    local file = io.open(name, "w")
    if not file then
        log("could not write " .. name)
        return
    end
    file:write(content)
    file:close()
    log("wrote " .. name)
end

-- Runs fn and appends "label: value" (or the error) to out.
local function field(out, label, fn)
    local ok, value = pcall(fn)
    table.insert(out, label .. ": " .. (ok and tostring(value) or ("error: " .. tostring(value))))
    return ok, value
end

local function nameOf(value)
    if value == nil then
        return "nil"
    end
    local ok, text = pcall(function() return value.value end)
    if ok and text then
        return text
    end
    ok, text = pcall(function() return Game.NameToString(value) end)
    if ok and text then
        return text
    end
    return tostring(value)
end

local function tdbName(id)
    local ok, text = pcall(function() return TDBID.ToStringDEBUG(id) end)
    if ok and text then
        return text
    end
    return tostring(id)
end

local function fmt(v)
    return string.format("(%.2f, %.2f, %.2f)", v.x, v.y, v.z)
end

local function dumpTypes(fileName, typeNames)
    local out = {}
    for _, typeName in ipairs(typeNames) do
        local ok, text = pcall(function() return DumpType(typeName, true) end)
        table.insert(out, "==== " .. typeName .. "\n" .. tostring(ok and text or ("error: " .. tostring(text))))
    end
    writeFile(fileName, table.concat(out, "\n\n"))
end

local function later(seconds, label, fn)
    table.insert(panel.checks, { at = os.clock() + seconds, label = label, fn = fn })
end

-- The object under V's crosshair, or nil.
local function lookAtObject()
    local ok, object = pcall(function()
        return Game.GetTargetingSystem():GetLookAtObject(Game.GetPlayer(), false, false)
    end)
    if ok then
        return object
    end
    return nil
end

-- ---------------------------------------------------------------------------------------------------
-- Access to the plugin (native system first, script bridge as a fallback)

local function bridge()
    local ok, system = pcall(function()
        return Game.GetScriptableSystemsContainer():Get("CoopBridge")
    end)
    if ok then
        return system
    end
    return nil
end

-- Returns an object with Host/Join/Leave/GetStatus/SetImpairment, from the native system if CET can see
-- it, otherwise through the script bridge's pass-through methods.
local function coopSystem()
    local ok, system = pcall(function() return Game.GetCoopSystem() end)
    if ok and system then
        return system
    end

    local b = bridge()
    if not b then
        return nil
    end
    return {
        Host = function(_, port, password) return b:DevHost(port, password) end,
        Join = function(_, address, password) return b:DevJoin(address, password) end,
        Leave = function(_) b:DevLeave() end,
        GetStatus = function(_) return b:DevStatus() end,
        SetImpairment = function(_, preset) return b:DevImpairment(preset) end,
    }
end

local function entitySystem()
    return Game.GetDynamicEntitySystem()
end

-- ---------------------------------------------------------------------------------------------------
-- Note on engine names: in CET an empty string passed to CName.new is NOT the engine's empty name (only
-- "None" is), so passing "" as an ease curve makes the engine look up a curve that doesn't exist; that crashed
-- the game in the first S8 run. Optional curves are left out, or passed as CName.new("None") when a later
-- argument is needed. (Applying time fields to the game is built into the mod since round C: CoopBridge.reds.)

-- ---------------------------------------------------------------------------------------------------
-- Probes

local function spawnInFront(recordName, distance)
    local player = Game.GetPlayer()
    local position = player:GetWorldPosition()
    local forward = player:GetWorldForward()
    position.x = position.x + forward.x * distance
    position.y = position.y + forward.y * distance

    local spec = DynamicEntitySpec.new()
    spec.recordID = TweakDBID.new(recordName)
    spec.position = position
    spec.orientation = player:GetWorldOrientation()
    spec.alwaysSpawned = true
    spec.tags = { CName.new("CoopProbe") }
    return entitySystem():CreateEntity(spec)
end

local function probeEntities()
    return entitySystem():GetTagged(CName.new("CoopProbe"))
end

local function probeVehicles()
    return entitySystem():GetTagged(CName.new("CoopProbeVehicle"))
end

local probes = {}

-- S1: records and spawning -------------------------------------------------------------------------------

probes.s1ListRecords = function()
    local patterns = { "TPP_Player", "Player_Puppet", "Character%.Player", "_Cutscene_", "Character%.Silverhand" }
    local found = {}
    for _, record in ipairs(TweakDB:GetRecords("gamedataCharacter_Record")) do
        local name = TDBID.ToStringDEBUG(record:GetID())
        for _, pattern in ipairs(patterns) do
            if name and string.find(name, pattern) then
                table.insert(found, name)
                break
            end
        end
    end
    table.sort(found)
    writeFile("probe-s1-records.txt", table.concat(found, "\n"))
    return #found .. " candidate records"
end

probes.s1Spawn = function()
    return spawnInFront(panel.probeRecord, 3.0)
end

-- The bridge spawns one lookalike per remote player. Can two copies of V exist at the same time?
-- (Round C: a female lookalike doesn't spawn for a male V, so both use the Record field.)
probes.s1SpawnPair = function()
    local a = spawnInFront(panel.probeRecord, 3.0)
    local b = spawnInFront(panel.probeRecord, 5.0)
    later(10.0, "S1 two lookalikes after 10 s", function()
        local des = entitySystem()
        return string.format("first there %s, second there %s", tostring(des:GetEntity(a) ~= nil),
            tostring(des:GetEntity(b) ~= nil))
    end)
    return "spawned two of " .. panel.probeRecord .. "; check in 10 s"
end

-- In a session the bridge's lookalikes never appeared, while these probes' do (round C). The old bridge differed in
-- two ways: the record ID came from TDBID.Create, and it set spawnInView, persistState and persistSpawn. This spawns
-- one lookalike per combination and says after 10 s which ones are there.
probes.s1SpawnLikeBridge = function()
    local player = Game.GetPlayer()
    local forward = player:GetWorldForward()
    local variants = {
        { label = "TDBID.Create, default settings", create = true, oldFlags = false },
        { label = "TweakDBID.new, old bridge settings", create = false, oldFlags = true },
        { label = "TDBID.Create, old bridge settings (= old bridge)", create = true, oldFlags = true },
    }
    local ids = {}
    for i, variant in ipairs(variants) do
        local position = player:GetWorldPosition()
        position.x = position.x + forward.x * (1.0 + 2.0 * i)
        position.y = position.y + forward.y * (1.0 + 2.0 * i)
        local spec = DynamicEntitySpec.new()
        if variant.create then
            spec.recordID = TDBID.Create(panel.probeRecord)
        else
            spec.recordID = TweakDBID.new(panel.probeRecord)
        end
        spec.position = position
        spec.orientation = player:GetWorldOrientation()
        spec.alwaysSpawned = true
        if variant.oldFlags then
            spec.spawnInView = true
            spec.persistState = false
            spec.persistSpawn = false
        end
        spec.tags = { CName.new("CoopProbe") }
        ids[i] = entitySystem():CreateEntity(spec)
    end
    later(10.0, "S1 spawn variants after 10 s", function()
        local des = entitySystem()
        local parts = {}
        for i, variant in ipairs(variants) do
            table.insert(parts, string.format("%d m: %s -> %s", 1 + 2 * i, variant.label,
                des:GetEntity(ids[i]) ~= nil and "THERE" or "missing"))
        end
        return table.concat(parts, "; ")
    end)
    return "spawned 3 lookalikes at 3, 5 and 7 m; result in 10 s"
end

probes.s1DumpTypes = function()
    dumpTypes("probe-s1-types.txt", { "gameuiCharacterCustomizationSystem", "PlayerPuppet", "gamePuppet", "NPCPuppet" })
end

-- S1b: why a V-lookalike copies the local V, and whether it can wear its own things -----------------------

local interestingComponent = { "ustomiz", "ppearance", "Photo", "Mirror", "Replac", "Slot", "Equip", "Inventory",
    "Item", "Morph", "Garment", "Player", "Cutscene" }

local function listComponents(out, label, entity, classes)
    table.insert(out, "==== " .. label)
    field(out, "class", function() return nameOf(entity:GetClassName()) end)
    field(out, "record", function() return tdbName(entity:GetRecordID()) end)
    field(out, "appearance", function() return nameOf(entity:GetCurrentAppearanceName()) end)
    field(out, "has character customization component", function()
        return Game.GetCharacterCustomizationSystem():HasCharacterCustomizationComponent(entity)
    end)
    local ok, components = pcall(function() return entity:GetComponents() end)
    if not ok or components == nil then
        table.insert(out, "GetComponents failed: " .. tostring(components))
        return
    end
    table.insert(out, #components .. " components:")
    for _, component in ipairs(components) do
        local className = nameOf(component:GetClassName())
        table.insert(out, "  " .. className .. "  " .. nameOf(component:GetName()))
        classes[className] = true
    end
end

probes.s1bComponents = function()
    local out = {}
    local classes = {}
    for i, entity in ipairs(probeEntities()) do
        listComponents(out, "probe NPC " .. i, entity, classes)
    end
    listComponents(out, "V (for comparison)", Game.GetPlayer(), {})

    -- Dump the component classes that could be what copies V's look or equipment.
    local names = {}
    for className in pairs(classes) do
        for _, pattern in ipairs(interestingComponent) do
            if string.find(className, pattern) then
                table.insert(names, className)
                break
            end
        end
    end
    table.sort(names)
    table.insert(names, "gameuiICharacterCustomizationState")
    for _, typeName in ipairs(names) do
        local okDump, text = pcall(function() return DumpType(typeName, true) end)
        table.insert(out, "\n==== type " .. typeName .. "\n" .. tostring(okDump and text or ("error: " .. tostring(text))))
    end
    writeFile("probe-s1b-components.txt", table.concat(out, "\n"))
    return #probeEntities() .. " probe NPC(s) listed"
end

local slotNames = { "WeaponRight", "WeaponLeft", "Head", "Face", "Eyes", "Chest", "Torso", "Legs", "Feet", "Outfit" }

local function listItems(out, label, object)
    local transactions = Game.GetTransactionSystem()
    table.insert(out, "==== " .. label)
    for _, slot in ipairs(slotNames) do
        field(out, "slot " .. slot, function()
            local item = transactions:GetItemInSlot(object, TweakDBID.new("AttachmentSlots." .. slot))
            if item == nil then
                return "(empty)"
            end
            return tdbName(item:GetItemID().id)
        end)
    end
    local ok, success, items = pcall(function() return transactions:GetItemList(object) end)
    if ok and type(items) == "table" then
        table.insert(out, #items .. " item(s) in its inventory:")
        for _, data in ipairs(items) do
            local okName, name = pcall(function() return tdbName(data:GetID().id) end)
            table.insert(out, "  " .. (okName and name or "?"))
        end
    else
        table.insert(out, "GetItemList: " .. tostring(ok and success or success))
    end
end

probes.s1bItems = function()
    local out = {}
    for i, entity in ipairs(probeEntities()) do
        listItems(out, "probe NPC " .. i, entity)
    end
    listItems(out, "V", Game.GetPlayer())
    writeFile("probe-s1b-items.txt", table.concat(out, "\n"))
end

-- Can a V-lookalike hold or wear something V doesn't? (S1: it copies V's weapon.)
probes.s1bPutItem = function()
    local transactions = Game.GetTransactionSystem()
    local itemID = ItemID.FromTDBID(TweakDBID.new(panel.itemRecord))
    local slotName = panel.itemSlot
    local slot = TweakDBID.new(slotName)
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        log("S1b: GiveItem(" .. panel.itemRecord .. ") to a probe NPC")
        transactions:GiveItem(entity, itemID, 1)
        log("S1b: AddItemToSlot(" .. slotName .. ")")
        local placed = transactions:AddItemToSlot(entity, slot, itemID)
        log("S1b: AddItemToSlot returned " .. tostring(placed))
        count = count + 1
        later(2.0, "S1b item check", function()
            local item = transactions:GetItemInSlot(entity, slot)
            return slotName .. " on the probe now holds " .. (item and tdbName(item:GetItemID().id) or "nothing")
        end)
    end
    return count .. " probe NPC(s); does it show the item, and does it still copy V's?"
end

-- S2b: will enemies attack a puppet? -------------------------------------------------------------------------

probes.s2MakeAllies = function()
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        entity:GetAttitudeAgent():SetAttitudeGroup(CName.new("player"))
        count = count + 1
    end
    return count .. " probe NPC(s) set to attitude group 'player'"
end

probes.s2bDiagnose = function()
    local player = Game.GetPlayer()
    local npc = lookAtObject()
    local out = { "---- " .. os.date("%H:%M:%S") }
    field(out, "looking at", function()
        if npc == nil then
            return "nothing"
        end
        return nameOf(npc:GetClassName()) .. " " .. tdbName(npc:GetRecordID())
    end)
    field(out, "V attitude group", function() return nameOf(player:GetAttitudeAgent():GetAttitudeGroup()) end)
    if npc then
        field(out, "NPC attitude group", function() return nameOf(npc:GetAttitudeAgent():GetAttitudeGroup()) end)
        field(out, "NPC attitude towards V", function() return tostring(npc:GetAttitudeTowards(player)) end)
        field(out, "NPC in combat", function() return npc:IsInCombat() end)
    end
    for i, probe in ipairs(probeEntities()) do
        local p = "probe " .. i .. " "
        field(out, p .. "class", function() return nameOf(probe:GetClassName()) .. " " .. tdbName(probe:GetRecordID()) end)
        field(out, p .. "attitude group", function() return nameOf(probe:GetAttitudeAgent():GetAttitudeGroup()) end)
        field(out, p .. "can be seen (visible object)", function() return probe:GetVisibleObjectComponent() ~= nil end)
        field(out, p .. "has senses", function() return probe:GetSensesComponent() ~= nil end)
        field(out, p .. "has target tracker", function() return probe:GetTargetTrackerComponent() ~= nil end)
        field(out, p .. "attitude towards V", function() return tostring(probe:GetAttitudeTowards(player)) end)
        if npc then
            field(out, p .. "NPC attitude towards it", function() return tostring(npc:GetAttitudeTowards(probe)) end)
        end
    end
    local file = io.open("probe-s2b.txt", "a")
    if file then
        file:write(table.concat(out, "\n") .. "\n")
        file:close()
    end
    for _, line in ipairs(out) do
        log("S2b " .. line)
    end
    return "also appended to probe-s2b.txt"
end

probes.s2bHostile = function()
    local npc = lookAtObject()
    if npc == nil then
        return "put the crosshair on an NPC first"
    end
    local hostile = Enum.new("EAIAttitude", "AIA_Hostile")
    local count = 0
    for _, probe in ipairs(probeEntities()) do
        try("S2b NPC hostile towards probe", function()
            npc:GetAttitudeAgent():SetAttitudeTowards(probe:GetAttitudeAgent(), hostile)
            return tostring(npc:GetAttitudeTowards(probe))
        end)
        try("S2b probe hostile towards NPC", function()
            probe:GetAttitudeAgent():SetAttitudeTowards(npc:GetAttitudeAgent(), hostile)
            return tostring(probe:GetAttitudeTowards(npc))
        end)
        count = count + 1
    end
    return count .. " probe NPC(s); does the NPC go after it now?"
end

probes.s2bThreat = function()
    local npc = lookAtObject()
    if npc == nil then
        return "put the crosshair on an NPC first"
    end
    local count = 0
    for _, probe in ipairs(probeEntities()) do
        try("S2b TargetTrackingExtension.InjectThreat(npc, probe)", function()
            TargetTrackingExtension.InjectThreat(npc, probe)
        end)
        try("S2b target tracker AddThreat(probe)", function()
            return npc:GetTargetTrackerComponent():AddThreat(probe, true, probe:GetWorldPosition(), 1.0, 30.0, false)
        end)
        count = count + 1
    end
    return count .. " probe NPC(s); does the NPC shoot at it now?"
end

-- S2c: which of the two threat calls makes an NPC attack? (Round C: both together did.)
local function threatOnly(useInject)
    local npc = lookAtObject()
    if npc == nil then
        return "put the crosshair on an NPC first"
    end
    local count = 0
    for _, probe in ipairs(probeEntities()) do
        if useInject then
            TargetTrackingExtension.InjectThreat(npc, probe)
        else
            npc:GetTargetTrackerComponent():AddThreat(probe, true, probe:GetWorldPosition(), 1.0, 30.0, false)
        end
        count = count + 1
    end
    return count .. " probe(s); does the NPC attack now?"
end

probes.s2cInjectOnly = function() return threatOnly(true) end
probes.s2cAddOnly = function() return threatOnly(false) end

-- S1c: the impostor component is what copies the local V onto a lookalike (round C). What does it hold, and does
-- the lookalike keep its look when it's switched off?
local function impostorsOf(entity)
    local found = {}
    local ok, components = pcall(function() return entity:GetComponents() end)
    if ok and components then
        for _, component in ipairs(components) do
            if nameOf(component:GetClassName()) == "gameImpostorComponent" then
                table.insert(found, component)
            end
        end
    end
    return found
end

probes.s1cImpostorDump = function()
    local out = {}
    local okType, typeText = pcall(function() return DumpType("gameImpostorComponent", true) end)
    table.insert(out, "==== type gameImpostorComponent\n" .. tostring(okType and typeText or ("error: " .. tostring(typeText))))
    for i, entity in ipairs(probeEntities()) do
        for _, component in ipairs(impostorsOf(entity)) do
            local okDump, text = pcall(function() return Dump(component, true) end)
            table.insert(out, "==== probe " .. i .. " impostor\n" .. tostring(okDump and text or ("error: " .. tostring(text))))
        end
    end
    writeFile("probe-s1c-impostor.txt", table.concat(out, "\n\n"))
end

probes.s1cImpostorOff = function()
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        for _, component in ipairs(impostorsOf(entity)) do
            log("S1c: Toggle(false) on a lookalike's impostor")
            component:Toggle(false)
            count = count + 1
        end
    end
    return count .. " impostor(s) switched off; now change V's jacket or weapon: does the lookalike still follow?"
end

-- Round E ------------------------------------------------------------------------------------------------------
-- S3d: less delay than AI walking. (1) an AI-side teleport, for snapping a puppet into place instead of respawning
-- it; (2) the AI switched off and the NPC moved by a teleport every frame, the "direct drive" a multiplayer body
-- would ideally use; (3) the AI following a moving target with matched speed.

local function componentsOfClass(entity, className)
    local found = {}
    local ok, components = pcall(function() return entity:GetComponents() end)
    if ok and components then
        for _, component in ipairs(components) do
            if nameOf(component:GetClassName()) == className then
                table.insert(found, component)
            end
        end
    end
    return found
end

probes.s3dAITeleport = function()
    local forward = Game.GetPlayer():GetWorldForward()
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        local before = entity:GetWorldPosition()
        local command = NewObject("handle:AITeleportCommand")
        command.position = Vector4.new(before.x + forward.x * 5.0, before.y + forward.y * 5.0, before.z, 1.0)
        command.rotation = 0.0
        command.doNavTest = false
        entity:GetAIControllerComponent():SendCommand(command)
        count = count + 1
        later(1.0, "S3d AI teleport result", function()
            return string.format("moved %.2f m (asked 5)", Vector4.Distance(before, entity:GetWorldPosition()))
        end)
    end
    return count .. " AI teleport command(s) sent; result in 1 s"
end

probes.s3dDirectDrive = function()
    local entities = probeEntities()
    if #entities == 0 then
        return "spawn a probe NPC first"
    end
    local entity = entities[1]
    local ai = componentsOfClass(entity, "AIHumanComponent")
    for _, component in ipairs(ai) do
        log("S3d: Toggle(false) on the probe's AI component")
        component:Toggle(false)
    end
    local forward = Game.GetPlayer():GetWorldForward()
    local atan2 = math.atan2 or math.atan
    panel.directDrive = {
        entity = entity,
        ai = ai,
        start = entity:GetWorldPosition(),
        forward = forward,
        yaw = math.deg(atan2(-forward.x, forward.y)),
        began = os.clock() + 1.0, -- give the AI a moment to stop
        duration = 4.0,
        speed = 3.0,
        frames = 0,
    }
    return #ai .. " AI component(s) off; in 1 s the probe is moved 12 m forward over 4 s, one teleport per frame"
end

local function updateDirectDrive()
    local d = panel.directDrive
    if not d or os.clock() < d.began then
        return
    end
    local elapsed = os.clock() - d.began
    local t = math.min(elapsed, d.duration)
    local ok, err = pcall(function()
        local target = Vector4.new(d.start.x + d.forward.x * d.speed * t, d.start.y + d.forward.y * d.speed * t, d.start.z, 1.0)
        Game.GetTeleportationFacility():Teleport(d.entity, target, EulerAngles.new(0, 0, d.yaw))
    end)
    d.frames = d.frames + 1
    if not ok then
        log("S3d direct drive failed: " .. tostring(err))
        panel.directDrive = nil
    elseif elapsed >= d.duration then
        panel.directDrive = nil
        later(0.5, "S3d direct drive result", function()
            local moved = Vector4.Distance(d.start, d.entity:GetWorldPosition())
            for _, component in ipairs(d.ai) do
                component:Toggle(true)
            end
            return string.format("%d teleports, moved %.2f m (expected 12), AI back on. Did it move smoothly? Legs moving?",
                d.frames, moved)
        end)
    end
end

probes.s3dFollowV = function()
    panel.follow = panel.follow or {}
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        local command = NewObject("handle:AIFollowTargetCommand")
        command.target = Game.GetPlayer()
        command.desiredDistance = 2.0
        command.tolerance = 0.5
        command.stopWhenDestinationReached = false
        command.movementType = "Run"
        command.matchSpeed = true
        command.teleport = false
        entity:GetAIControllerComponent():SendCommand(command)
        table.insert(panel.follow, { entity = entity, command = command })
        count = count + 1
    end
    return count .. " probe(s) following you; walk, run, sprint and stop: do they keep close, at your speed?"
end

probes.s3dStopFollow = function()
    local count = 0
    for _, entry in ipairs(panel.follow or {}) do
        pcall(function() entry.entity:GetAIControllerComponent():CancelCommand(entry.command) end)
        count = count + 1
    end
    panel.follow = {}
    return count .. " follow command(s) cancelled"
end

-- S2d: puppets must not fight on their own (round D: a plain-NPC puppet joined a fight by itself).
probes.s2dPassive = function()
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        log("S2d: EnableSensesComponent(false) on a probe")
        entity:EnableSensesComponent(false)
        count = count + 1
    end
    return count .. " probe(s) with senses off: Diagnose (can it still be seen?), then get into a fight next to it"
end

-- S1c: the impostor's settings. slotIDsToOmit looks like a list of slots it does NOT copy from V.
probes.s1cImpostorValues = function()
    local count = 0
    for i, entity in ipairs(probeEntities()) do
        for _, component in ipairs(componentsOfClass(entity, "gameImpostorComponent")) do
            local slots = {}
            pcall(function()
                for _, id in ipairs(component.slotIDsToOmit) do
                    table.insert(slots, tdbName(id))
                end
            end)
            log(string.format("S1c probe %d impostor: enabled %s, isCharacterReplica %s, addHead %s, ignorePlayerHeadSlot %s, slotIDsToOmit [%s]",
                i, tostring(component:IsEnabled()), tostring(component.isCharacterReplica), tostring(component.addHead),
                tostring(component.ignorePlayerHeadSlot), table.concat(slots, ", ")))
            count = count + 1
        end
    end
    return count .. " impostor(s) listed"
end

probes.s1cOmitWeapons = function()
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        for _, component in ipairs(componentsOfClass(entity, "gameImpostorComponent")) do
            log("S1c: slotIDsToOmit = WeaponRight, WeaponLeft; Toggle off and on")
            component.slotIDsToOmit = { TweakDBID.new("AttachmentSlots.WeaponRight"), TweakDBID.new("AttachmentSlots.WeaponLeft") }
            component:Toggle(false)
            component:Toggle(true)
            count = count + 1
        end
    end
    return count .. " impostor(s) told not to copy weapons; switch V's weapon: does the lookalike still follow?"
end

-- S3: driving an NPC ------------------------------------------------------------------------------------------

probes.s3WalkToPlayer = function()
    local player = Game.GetPlayer()
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        local destination = NewObject("WorldPosition")
        destination:SetVector4(destination, player:GetWorldPosition())
        local spec = NewObject("AIPositionSpec")
        spec:SetWorldPosition(spec, destination)
        local command = NewObject("handle:AIMoveToCommand")
        command.movementTarget = spec
        command.movementType = "Walk"
        command.ignoreNavigation = true
        command.desiredDistanceFromTarget = 1.5
        command.finishWhenDestinationReached = true
        entity:GetAIControllerComponent():SendCommand(command)
        count = count + 1
    end
    return count .. " AI move command(s) sent"
end

local function sendMove(entity, position, movementType)
    local destination = NewObject("WorldPosition")
    destination:SetVector4(destination, position)
    local spec = NewObject("AIPositionSpec")
    spec:SetWorldPosition(spec, destination)
    local command = NewObject("handle:AIMoveToCommand")
    command.movementTarget = spec
    command.movementType = movementType
    command.ignoreNavigation = true
    command.desiredDistanceFromTarget = 0.5
    command.finishWhenDestinationReached = true
    entity:GetAIControllerComponent():SendCommand(command)
end

-- Round C: the V-lookalike moves with AI commands but doesn't animate. Which V-like body does? Spawns one of each
-- in a row 5 m ahead of V (numbered from the left; the last one is the plain NPC that animated in S3, as a
-- control), and after 6 s walks them all 12 m forward side by side.
local function lineupRecords()
    local gender = "Male"
    pcall(function()
        if nameOf(Game.GetPlayer():GetResolvedGenderName()) == "Female" then
            gender = "Female"
        end
    end)
    return {
        "Character.TPP_Player_Cutscene_" .. gender,
        "Character.TPP_Player_Cutscene_No_Impostor_" .. gender,
        "Character.TPP_Player",
        "Character.Player_Puppet_Photomode",
        "Character.Player_Replacer_Puppet_Base",
        "Cp2077Coop.Character.RemotePlayer",
    }
end

probes.s3bLineup = function()
    local player = Game.GetPlayer()
    local origin = player:GetWorldPosition()
    local forward = player:GetWorldForward()
    local right = { x = forward.y, y = -forward.x }
    local records = lineupRecords()
    local lineup = {}
    for i, record in ipairs(records) do
        local offset = (i - (#records + 1) / 2) * 2.5
        local position = Vector4.new(origin.x + forward.x * 5.0 + right.x * offset,
            origin.y + forward.y * 5.0 + right.y * offset, origin.z, 1.0)
        local spec = DynamicEntitySpec.new()
        spec.recordID = TweakDBID.new(record)
        spec.position = position
        spec.orientation = player:GetWorldOrientation()
        spec.alwaysSpawned = true
        spec.tags = { CName.new("CoopProbe") }
        local ok, id = pcall(function() return entitySystem():CreateEntity(spec) end)
        table.insert(lineup, { number = i, record = record, id = ok and id or nil })
        log(string.format("S3b lineup %d (from the left): %s", i, record))
    end

    later(6.0, "S3b lineup walk", function()
        local parts = {}
        for _, member in ipairs(lineup) do
            local entity = member.id and entitySystem():GetEntity(member.id)
            if entity then
                member.start = entity:GetWorldPosition()
                local target = Vector4.new(member.start.x + forward.x * 12.0, member.start.y + forward.y * 12.0,
                    member.start.z, 1.0)
                local ok, err = pcall(function() sendMove(entity, target, "Walk") end)
                table.insert(parts, member.number .. (ok and ": walking" or (": can't walk (" .. tostring(err) .. ")")))
            else
                table.insert(parts, member.number .. ": didn't spawn")
            end
        end
        later(12.0, "S3b lineup result", function()
            local results = {}
            for _, member in ipairs(lineup) do
                local entity = member.id and entitySystem():GetEntity(member.id)
                if entity and member.start then
                    table.insert(results, string.format("%d moved %.1f m", member.number,
                        Vector4.Distance(member.start, entity:GetWorldPosition())))
                end
            end
            return table.concat(results, ", ") .. ". Which numbers had walking animations?"
        end)
        return table.concat(parts, ", ")
    end)
    return #records .. " bodies spawning 5 m ahead, numbered from the left; in 6 s they walk 12 m forward"
end

-- S3c: why do V-lookalikes slide while the plain NPC animates? Dumps the animation, movement and AI components of
-- every probe (with their settings) so the two kinds of body can be compared. Run it with a lineup standing.
local animComponent = { "Anim", "Move", "AI", "Locomotion", "Character", "Physic", "Skinned", "Slot" }

probes.s3cAnimDump = function()
    local out = {}
    for i, entity in ipairs(probeEntities()) do
        table.insert(out, "######## probe " .. i .. ": " .. tdbName(entity:GetRecordID()))
        local ok, components = pcall(function() return entity:GetComponents() end)
        if ok and components then
            for _, component in ipairs(components) do
                local className = nameOf(component:GetClassName())
                local wanted = false
                for _, pattern in ipairs(animComponent) do
                    if string.find(className, pattern) then
                        wanted = true
                        break
                    end
                end
                if wanted then
                    local okDump, text = pcall(function() return Dump(component, true) end)
                    table.insert(out, "==== " .. className .. " " .. nameOf(component:GetName()) .. "\n"
                        .. tostring(okDump and text or ("error: " .. tostring(text))))
                end
            end
        else
            table.insert(out, "GetComponents failed: " .. tostring(components))
        end
    end
    writeFile("probe-s3c-anim.txt", table.concat(out, "\n"))
    return #probeEntities() .. " probe(s) dumped"
end

probes.s3DumpTypes = function()
    dumpTypes("probe-s3-types.txt", { "AIHumanComponent", "AIMoveToCommand", "AnimFeature_Locomotion", "gameTeleportationFacility" })
end

-- S8: time dilation -------------------------------------------------------------------------------------------

probes.s8DumpTypes = function()
    dumpTypes("probe-s8-types.txt", { "gameTimeSystem", "TimeDilationHelper", "gameTimeDilatable", "PlayerPuppet" })
end

probes.s8WorldSlowmo = function()
    local timeSystem = Game.GetTimeSystem()
    log("S8: SetTimeDilation(coopProbe, 0.25, 3 s)")
    timeSystem:SetTimeDilation(CName.new("coopProbe"), 0.25, 3.0)
    log("S8: SetTimeDilation returned")
    panel.pendingUnset = os.clock() + 3.5
    panel.pendingUnsetExempt = false
    return "world at 0.25 for 3 s: does everything, V included, slow down?"
end

probes.s8WorldSlowmoExempt = function()
    local timeSystem = Game.GetTimeSystem()
    log("S8: SetIgnoreTimeDilationOnLocalPlayerZero(true)")
    timeSystem:SetIgnoreTimeDilationOnLocalPlayerZero(true)
    log("S8: SetIgnoreTimeDilationOnLocalPlayerZero returned")
    log("S8: SetTimeDilation(coopProbe, 0.25, 3 s)")
    timeSystem:SetTimeDilation(CName.new("coopProbe"), 0.25, 3.0)
    log("S8: SetTimeDilation returned")
    panel.pendingUnset = os.clock() + 3.5
    panel.pendingUnsetExempt = true
    return "world at 0.25 for 3 s with V exempt: does V move at full speed?"
end

probes.s8IndividualFast = function()
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        log("S8: SetIndividualTimeDilation(coopProbe, 2.0, 3 s) on a probe NPC")
        entity:SetIndividualTimeDilation(CName.new("coopProbe"), 2.0, 3.0)
        log("S8: SetIndividualTimeDilation returned")
        count = count + 1
    end
    return count .. " probe NPC(s) at individual rate 2.0 for 3 s"
end

-- S8b: a puppet keeps full speed while the world is slowed (how a remote Sandevistan user must look).
probes.s8bPuppetFullSpeed = function()
    local entities = probeEntities()
    if #entities == 0 then
        return "spawn a probe NPC first and walk ~15 m away from it"
    end
    probes.s3WalkToPlayer()
    local none = CName.new("None")
    for _, entity in ipairs(entities) do
        log("S8b: SetIndividualTimeDilation(coopProbe, 1.0, 6 s, None, None, ignore world) on a probe NPC")
        entity:SetIndividualTimeDilation(CName.new("coopProbe"), 1.0, 6.0, none, none, true)
        log("S8b: SetIndividualTimeDilation returned")
    end
    log("S8b: SetTimeDilation(coopProbe, 0.25, 4 s)")
    Game.GetTimeSystem():SetTimeDilation(CName.new("coopProbe"), 0.25, 4.0)
    log("S8b: SetTimeDilation returned")
    panel.pendingUnset = os.clock() + 4.5
    panel.pendingUnsetExempt = false
    later(5.0, "S8b individual unset", function()
        for _, entity in ipairs(probeEntities()) do
            log("S8b: UnsetIndividualTimeDilation on a probe NPC")
            entity:UnsetIndividualTimeDilation()
        end
        return "back to normal"
    end)
    return "world at 0.25 for 4 s: does the probe NPC keep walking at normal speed while everything else is slow?"
end

-- S1 vehicles ------------------------------------------------------------------------------------------------

probes.s1vSpawn = function()
    local player = Game.GetPlayer()
    local position = player:GetWorldPosition()
    local forward = player:GetWorldForward()
    position.x = position.x + forward.x * 8.0
    position.y = position.y + forward.y * 8.0

    local spec = DynamicEntitySpec.new()
    spec.recordID = TweakDBID.new(panel.vehicleRecord)
    spec.position = position
    spec.orientation = player:GetWorldOrientation()
    spec.alwaysSpawned = true
    spec.tags = { CName.new("CoopProbeVehicle") }
    return entitySystem():CreateEntity(spec)
end

-- Does the teleportation facility move a vehicle (and anyone seated in it)? Checked 1 s later.
probes.s1vTeleport = function()
    local count = 0
    for _, vehicle in ipairs(probeVehicles()) do
        local before = vehicle:GetWorldPosition()
        local target = vehicle:GetWorldPosition()
        local forward = vehicle:GetWorldForward()
        target.x = target.x + forward.x * 5.0
        target.y = target.y + forward.y * 5.0
        Game.GetTeleportationFacility():Teleport(vehicle, target, EulerAngles.new(0, 0, 0))
        count = count + 1
        later(1.0, "S1v teleport result", function()
            local after = vehicle:GetWorldPosition()
            local moved = Vector4.Distance(before, after)
            local text = string.format("before %s target %s after %s: moved %.2f m (expected ~5)", fmt(before),
                fmt(target), fmt(after), moved)
            for _, npc in ipairs(probeEntities()) do
                text = text .. string.format("; probe NPC %.2f m from the car", Vector4.Distance(npc:GetWorldPosition(), after))
            end
            return text
        end)
    end
    return count .. " vehicle(s) told to move 5 m forward; result in 1 s"
end

-- How a remote player's car would be moved: a teleport every frame along a line. Smooth, or jittery?
probes.s1vDrive = function()
    local vehicles = probeVehicles()
    if #vehicles == 0 then
        return "spawn a probe vehicle first"
    end
    local vehicle = vehicles[1]
    local forward = vehicle:GetWorldForward()
    local atan2 = math.atan2 or math.atan
    panel.drive = {
        vehicle = vehicle,
        start = vehicle:GetWorldPosition(),
        forward = forward,
        yaw = math.deg(atan2(-forward.x, forward.y)),
        began = os.clock(),
        duration = 3.0,
        speed = 5.0,
        frames = 0,
    }
    return "moving the probe vehicle 15 m forward over 3 s, one teleport per frame"
end

local function seatProbeNpc(slotName)
    local vehicles = probeVehicles()
    local npcs = probeEntities()
    if #vehicles == 0 or #npcs == 0 then
        return "spawn a probe vehicle and a probe NPC first"
    end
    local vehicle = vehicles[1]
    local npc = npcs[1]

    local mountData = NewObject("handle:gameMountEventData")
    mountData.mountParentEntityId = vehicle:GetEntityID()
    mountData.slotName = CName.new(slotName)
    mountData.isInstant = true
    mountData.ignoreHLS = true
    local command = NewObject("handle:AIMountCommand")
    command.mountData = mountData
    npc:GetAIControllerComponent():SendCommand(command)

    later(2.0, "S1v seat result (" .. slotName .. ")", function()
        local distance = Vector4.Distance(npc:GetWorldPosition(), vehicle:GetWorldPosition())
        return string.format("NPC is %.2f m from the vehicle centre (seated is ~1 m or less)", distance)
    end)
    return "mount command sent; result in 2 s"
end

probes.s1vSeatNpc = function() return seatProbeNpc("seat_front_right") end
probes.s1vSeatDriver = function() return seatProbeNpc("seat_front_left") end

-- What the plugin would read from V's own car when V drives.
probes.s1vMine = function()
    local player = Game.GetPlayer()
    local vehicle = Game.GetMountedVehicle(player)
    if vehicle == nil then
        return "sit in a car first"
    end
    local out = {}
    field(out, "class", function() return nameOf(vehicle:GetClassName()) end)
    field(out, "record", function() return tdbName(vehicle:GetRecordID()) end)
    field(out, "appearance", function() return nameOf(vehicle:GetCurrentAppearanceName()) end)
    field(out, "V is driver", function() return vehicle:IsPlayerDriver() end)
    field(out, "V's seat", function() return nameOf(vehicle:GetSlotIdForMountedObject(player)) end)
    field(out, "speed", function() return string.format("%.1f", vehicle:GetCurrentSpeed()) end)
    field(out, "velocity", function() return fmt(vehicle:GetLinearVelocity()) end)
    field(out, "position", function() return fmt(vehicle:GetWorldPosition()) end)
    for _, line in ipairs(out) do
        log("S1v V's car " .. line)
    end
    return "see the lines above"
end

probes.s1vDumpTypes = function()
    dumpTypes("probe-s1v-types.txt", { "vehicleBaseObject", "vehicleCarBaseObject", "AIMountCommand", "gameMountEventData",
        "gameMountingFacility", "vehicleController", "VehicleComponent" })
end

-- One dump for everything round C needs to look up.
probes.cDumpTypes = function()
    dumpTypes("probe-c-types.txt", {
        "gameAttitudeAgent", "gameAttitudeSystem", "AITargetTrackerComponent", "TargetTrackingExtension",
        "senseVisibleObjectComponent", "senseComponent", "AIInjectCombatTargetCommand", "AIInjectCombatThreatCommand",
        "gameTargetingSystem", "WorldPosition", "AIPositionSpec", "ScriptedPuppet", "gameTransactionSystem",
        "gameuiICharacterCustomizationState", "gamemountingMountingFacility", "gamemountingMountingRequest",
        "AIRotateToCommand", "AIFollowTargetCommand", "AITeleportCommand",
    })
end

probes.cleanup = function()
    panel.drive = nil
    panel.directDrive = nil
    panel.follow = {}
    entitySystem():DeleteTagged(CName.new("CoopProbe"))
    entitySystem():DeleteTagged(CName.new("CoopProbeVehicle"))
end

-- ---------------------------------------------------------------------------------------------------
-- UI

-- What Codeware's entity system says about each puppet: is it known, still spawning, spawned? Each call is
-- wrapped, so a function this Codeware version doesn't have just shows "?".
local function puppetSpawnStates(b)
    local lines = {}
    local des = entitySystem()
    local function ask(fn)
        local ok, value = pcall(fn)
        return ok and tostring(value) or "?"
    end
    for i = 0, b:GetPuppetCount() - 1 do
        local okId, id = pcall(function() return b:GetPuppetIdAt(i) end)
        if okId and id then
            table.insert(lines, string.format("puppet %d (id %s): managed %s, spawning %s, spawned %s, entity %s", i + 1,
                ask(function() return id.hash end), ask(function() return des:IsManaged(id) end),
                ask(function() return des:IsSpawning(id) end), ask(function() return des:IsSpawned(id) end),
                ask(function() return des:GetEntity(id) ~= nil end)))
        end
    end
    return lines
end

local function drawSession()
    local system = coopSystem()
    if not system then
        ImGui.TextWrapped("Co-op plugin not reachable. Load a save first; if it still fails, check that "
            .. "red4ext/plugins/Cp2077Coop/Cp2077Coop.dll loaded (red4ext/logs) and that redscript compiled.")
        return
    end

    panel.address = ImGui.InputText("Address", panel.address, 64)
    panel.password = ImGui.InputText("Password", panel.password, 64)
    panel.port = ImGui.InputInt("Host port", panel.port)

    if ImGui.Button("Host") then
        try("host", function() return system:Host(panel.port, panel.password) end)
    end
    ImGui.SameLine()
    if ImGui.Button("Join") then
        try("join", function() return system:Join(panel.address, panel.password) end)
    end
    ImGui.SameLine()
    if ImGui.Button("Leave") then
        try("leave", function() system:Leave() end)
    end

    local changed
    panel.impairment, changed = ImGui.Combo("Impairment", panel.impairment, impairments, #impairments)
    if changed then
        try("impairment " .. impairments[panel.impairment + 1], function()
            return system:SetImpairment(impairments[panel.impairment + 1])
        end)
    end

    ImGui.Separator()
    ImGui.TextWrapped(system:GetStatus())

    -- Time fields (Sandevistan): trigger one for V and watch the rates; bots nearby slow down with it.
    local okRates, world, localRate = pcall(function() return system:GetWorldRate(), system:GetLocalRate() end)
    if okRates then
        ImGui.Text(string.format("Time: world x%.2f, you x%.2f", world, localRate))
        if ImGui.Button("Sandevistan x0.25 for 8 s") then
            local _, id = try("sandevistan", function() return system:ActivateTimeField(0.25, 8.0) end)
            panel.lastActivation = id or 0
        end
        ImGui.SameLine()
        if ImGui.Button("Cancel") and panel.lastActivation ~= 0 then
            try("cancel sandevistan", function() system:CancelTimeField(panel.lastActivation) end)
        end
        ImGui.TextWrapped("The mod slows your game with the session (coop.ini [time] applyToGame).")
    end

    local b = bridge()
    if b then
        ImGui.Text("Puppets spawned: " .. tostring(b:GetPuppetCount()))
        local ok, debugText = pcall(function() return b:GetPuppetDebug() end)
        if ok and debugText and debugText ~= "" then
            ImGui.TextWrapped(debugText)
        end
        local okStates, states = pcall(function() return puppetSpawnStates(b) end)
        if okStates then
            for _, line in ipairs(states) do
                ImGui.TextWrapped(line)
            end
        end
        if ImGui.Button("Log puppet state") then
            log("puppets: " .. tostring(debugText))
            for _, line in ipairs(okStates and states or {}) do
                log(line)
            end
        end
    elseif not Game.GetPlayer() then
        ImGui.TextWrapped("Load a save: the script bridge starts with the game session.")
    else
        ImGui.TextWrapped("Script bridge not found: redscript did not load r6\\scripts\\Cp2077Coop. "
            .. "Check r6\\logs\\redscript_rCURRENT.log and that Codeware is installed.")
    end
end

local function probeButton(label, key)
    if ImGui.Button(label) then
        try(label, probes[key])
    end
end

local function drawProbes()
    ImGui.TextWrapped("Results go to probe-results.txt (and probe-*.txt files) in this mod's folder. "
        .. "Round C is at the top; earlier probes are at the bottom.")

    panel.probeRecord = ImGui.InputText("Record", panel.probeRecord, 128)
    probeButton("Spawn record in front of V", "s1Spawn")
    ImGui.SameLine()
    probeButton("Delete probe NPCs and cars", "cleanup")
    probeButton("Spawn two lookalikes at once", "s1SpawnPair")
    ImGui.SameLine()
    probeButton("Spawn the way the old bridge did", "s1SpawnLikeBridge")

    if ImGui.CollapsingHeader("S3b - which V-like body animates when walking?") then
        ImGui.TextWrapped("Stand somewhere flat and open, looking along a street.")
        probeButton("Line up V-like bodies and walk them", "s3bLineup")
        probeButton("Dump the probes' animation setup", "s3cAnimDump")
        probeButton("AI-walk probe NPCs to V", "s3WalkToPlayer")
    end
    if ImGui.CollapsingHeader("S1b - how a V lookalike gets its looks") then
        probeButton("List probe components", "s1bComponents")
        probeButton("List items: probes and V", "s1bItems")
        panel.itemRecord = ImGui.InputText("Item", panel.itemRecord, 128)
        panel.itemSlot = ImGui.InputText("Slot", panel.itemSlot, 128)
        probeButton("Put item on probe NPCs", "s1bPutItem")
    end
    if ImGui.CollapsingHeader("Round E") then
        ImGui.TextWrapped("S3d, with a plain NPC probe (Record Cp2077Coop.Character.RemotePlayer):")
        probeButton("AI teleport 5 m", "s3dAITeleport")
        probeButton("AI off, move by teleport every frame", "s3dDirectDrive")
        probeButton("Follow me with matched speed", "s3dFollowV")
        ImGui.SameLine()
        probeButton("Stop following", "s3dStopFollow")
        ImGui.TextWrapped("S2d:")
        probeButton("Make probes passive (senses off)", "s2dPassive")
        ImGui.TextWrapped("S1c, with a lookalike:")
        probeButton("List impostor settings", "s1cImpostorValues")
        probeButton("Impostor: don't copy weapons", "s1cOmitWeapons")
    end
    if ImGui.CollapsingHeader("Round D (optional)") then
        ImGui.TextWrapped("S2c: crosshair on an enemy, probe near it.")
        probeButton("Threat: InjectThreat only", "s2cInjectOnly")
        ImGui.SameLine()
        probeButton("Threat: AddThreat only", "s2cAddOnly")
        ImGui.TextWrapped("S1c: with a lookalike spawned.")
        probeButton("Dump the lookalike's impostor", "s1cImpostorDump")
        probeButton("Switch the lookalike's impostor off", "s1cImpostorOff")
    end
    if ImGui.CollapsingHeader("S2b - will enemies attack a puppet?") then
        ImGui.TextWrapped("Put the crosshair on an enemy first.")
        probeButton("Diagnose", "s2bDiagnose")
        probeButton("Make that NPC and the probes hostile", "s2bHostile")
        probeButton("Give that NPC the probes as threats", "s2bThreat")
        probeButton("Make probe NPCs player-aligned", "s2MakeAllies")
    end
    if ImGui.CollapsingHeader("S8b - puppet at full speed in slow motion") then
        probeButton("Probe keeps normal speed while the world is slow", "s8bPuppetFullSpeed")
    end
    if ImGui.CollapsingHeader("S1v-b - vehicles") then
        probeButton("Show V's car (sit in one first)", "s1vMine")
        panel.vehicleRecord = ImGui.InputText("Vehicle record", panel.vehicleRecord, 128)
        probeButton("Spawn vehicle in front of V", "s1vSpawn")
        probeButton("Seat probe NPC as driver", "s1vSeatDriver")
        ImGui.SameLine()
        probeButton("... as passenger", "s1vSeatNpc")
        probeButton("Move probe vehicle 5 m (teleport)", "s1vTeleport")
        probeButton("Drive probe vehicle 15 m (teleport every frame)", "s1vDrive")
    end
    probeButton("Dump types for round C", "cDumpTypes")

    ImGui.Separator()
    if ImGui.CollapsingHeader("Earlier probes (rounds A and B)") then
        probeButton("List candidate records", "s1ListRecords")
        probeButton("Dump customization types", "s1DumpTypes")
        probeButton("Dump AI and animation types", "s3DumpTypes")
        probeButton("World 0.25 for 3 s", "s8WorldSlowmo")
        probeButton("World 0.25 for 3 s, V exempt", "s8WorldSlowmoExempt")
        probeButton("Probe NPCs individual 2.0 for 3 s", "s8IndividualFast")
        probeButton("Dump time types", "s8DumpTypes")
        probeButton("Dump vehicle types", "s1vDumpTypes")
    end
end

registerForEvent("onInit", function()
    log("coop-dev loaded")
end)

registerForEvent("onOverlayOpen", function() panel.overlayOpen = true end)
registerForEvent("onOverlayClose", function() panel.overlayOpen = false end)

local function updateDrive()
    local d = panel.drive
    if not d then
        return
    end
    local elapsed = os.clock() - d.began
    local t = math.min(elapsed, d.duration)
    local ok, err = pcall(function()
        local target = Vector4.new(d.start.x + d.forward.x * d.speed * t, d.start.y + d.forward.y * d.speed * t, d.start.z, 1.0)
        Game.GetTeleportationFacility():Teleport(d.vehicle, target, EulerAngles.new(0, 0, d.yaw))
    end)
    d.frames = d.frames + 1
    if not ok then
        log("S1v drive failed: " .. tostring(err))
        panel.drive = nil
    elseif elapsed >= d.duration then
        panel.drive = nil
        later(0.5, "S1v drive result", function()
            local moved = Vector4.Distance(d.start, d.vehicle:GetWorldPosition())
            return string.format("%d teleports, moved %.2f m (expected 15). Did it glide, stutter or bounce?", d.frames, moved)
        end)
    end
end

registerForEvent("onUpdate", function()
    updateDrive()
    updateDirectDrive()

    local now = os.clock()
    for i = #panel.checks, 1, -1 do
        local check = panel.checks[i]
        if now >= check.at then
            table.remove(panel.checks, i)
            try(check.label, check.fn)
        end
    end

    if panel.pendingUnset and os.clock() >= panel.pendingUnset then
        panel.pendingUnset = nil
        local exempt = panel.pendingUnsetExempt
        try("unset slow-mo", function()
            local timeSystem = Game.GetTimeSystem()
            log("S8: UnsetTimeDilation(coopProbe)")
            timeSystem:UnsetTimeDilation(CName.new("coopProbe"))
            if exempt then
                log("S8: SetIgnoreTimeDilationOnLocalPlayerZero(false)")
                timeSystem:SetIgnoreTimeDilationOnLocalPlayerZero(false)
            end
            log("S8: back to normal")
        end)
    end
end)

registerForEvent("onDraw", function()
    if not panel.overlayOpen then
        return
    end
    if ImGui.Begin("Co-op (dev)") then
        if ImGui.BeginTabBar("coopTabs") then
            if ImGui.BeginTabItem("Session") then
                drawSession()
                ImGui.EndTabItem()
            end
            if ImGui.BeginTabItem("Spike probes") then
                drawProbes()
                ImGui.EndTabItem()
            end
            if ImGui.BeginTabItem("Log") then
                for _, line in ipairs(panel.lines) do
                    ImGui.TextWrapped(line)
                end
                ImGui.EndTabItem()
            end
            ImGui.EndTabBar()
        end
    end
    ImGui.End()
end)
