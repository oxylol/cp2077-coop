-- Cp2077Coop dev panel (Cyber Engine Tweaks). Development only; players never need this.
--
-- * Session controls: host / join / leave / impairment, and live status.
-- * Spike probes (docs/04-feasibility-and-risks.md §3): experiments whose results decide M0b/M1 code.
--   Every probe is wrapped in pcall, so a wrong guess about an engine API prints an error instead of
--   breaking anything. Results are appended to probe-results.txt in this folder; please send that file
--   back after running the probes.

local panel = {
    overlayOpen = false,
    address = "127.0.0.1:27077",
    password = "",
    port = 27077,
    impairment = 0,
    probeRecord = "Cp2077Coop.Character.RemotePlayer",
    lines = {},
    pendingUnset = nil,
    vehicleRecord = "Vehicle.v_standard2_archer_hella_player",
    applyTime = false,   -- experimental: apply time-field rates to the game (spike S8)
    timeApplied = false,
    lastActivation = 0,
    checks = {}, -- delayed probe checks: { at = os.clock() time, label, fn }
}

local impairments = { "none", "lan", "good", "typical", "bad", "awful" }

-- ---------------------------------------------------------------------------------------------------
-- Logging

local function log(text)
    local line = os.date("%H:%M:%S") .. "  " .. tostring(text)
    print("[coop-dev] " .. tostring(text))
    table.insert(panel.lines, line)
    if #panel.lines > 40 then
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

-- Experimental (spike S8): mirror the session's time-field rates onto the game's global time dilation, and
-- exempt V while activating. Uses the same calls as the S8 probes; switches itself off if a call fails.
local function applyTimeFields(system)
    local ok, err = pcall(function()
        local world = system:GetWorldRate()
        local timeSystem = Game.GetTimeSystem()
        if world < 0.999 then
            timeSystem:SetTimeDilation(CName.new("coopField"), world, 0.25, CName.new(""), CName.new(""))
            timeSystem:SetIgnoreTimeDilationOnLocalPlayerZero(system:IsActivatingTimeField())
            panel.timeApplied = true
        elseif panel.timeApplied then
            timeSystem:UnsetTimeDilation(CName.new("coopField"), CName.new(""))
            timeSystem:SetIgnoreTimeDilationOnLocalPlayerZero(false)
            panel.timeApplied = false
        end
    end)
    if not ok then
        panel.applyTime = false
        log("applying time fields failed, switched off: " .. tostring(err))
    end
end

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

local probes = {}

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

probes.s1DumpTypes = function()
    local out = {}
    for _, typeName in ipairs({ "gameuiCharacterCustomizationSystem", "PlayerPuppet", "gamePuppet", "NPCPuppet" }) do
        local ok, text = pcall(function() return DumpType(typeName, true) end)
        table.insert(out, "==== " .. typeName .. "\n" .. tostring(ok and text or ("error: " .. tostring(text))))
    end
    writeFile("probe-s1-types.txt", table.concat(out, "\n\n"))
end

probes.s2MakeAllies = function()
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        entity:GetAttitudeAgent():SetAttitudeGroup(CName.new("player"))
        count = count + 1
    end
    return count .. " probe NPC(s) set to attitude group 'player'"
end

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

probes.s3DumpTypes = function()
    local out = {}
    for _, typeName in ipairs({ "AIHumanComponent", "AIMoveToCommand", "AnimFeature_Locomotion", "gameTeleportationFacility" }) do
        local ok, text = pcall(function() return DumpType(typeName, true) end)
        table.insert(out, "==== " .. typeName .. "\n" .. tostring(ok and text or ("error: " .. tostring(text))))
    end
    writeFile("probe-s3-types.txt", table.concat(out, "\n\n"))
end

probes.s8DumpTypes = function()
    local out = {}
    for _, typeName in ipairs({ "gameTimeSystem", "TimeDilationHelper", "gameTimeDilatable", "PlayerPuppet" }) do
        local ok, text = pcall(function() return DumpType(typeName, true) end)
        table.insert(out, "==== " .. typeName .. "\n" .. tostring(ok and text or ("error: " .. tostring(text))))
    end
    writeFile("probe-s8-types.txt", table.concat(out, "\n\n"))
end

probes.s8WorldSlowmo = function()
    local timeSystem = Game.GetTimeSystem()
    timeSystem:SetIgnoreTimeDilationOnLocalPlayerZero(true)
    timeSystem:SetTimeDilation(CName.new("coopProbe"), 0.25, 3.0, CName.new(""), CName.new(""))
    panel.pendingUnset = os.clock() + 3.5
    return "world at 0.25 for 3 s with V exempt: does V move at full speed?"
end

probes.s8IndividualFast = function()
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        entity:SetIndividualTimeDilation(CName.new("coopProbe"), 2.0, 3.0, CName.new(""), CName.new(""))
        count = count + 1
    end
    return count .. " probe NPC(s) at individual rate 2.0 for 3 s"
end

-- S1, vehicles: spawn a proxy vehicle, move it from outside, seat an NPC in it (docs/02-systems.md §11).

local function probeVehicles()
    return entitySystem():GetTagged(CName.new("CoopProbeVehicle"))
end

local function later(seconds, label, fn)
    table.insert(panel.checks, { at = os.clock() + seconds, label = label, fn = fn })
end

local function fmt(v)
    return string.format("(%.2f, %.2f, %.2f)", v.x, v.y, v.z)
end

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

-- Does the teleportation facility move a vehicle? Checked 1 s later.
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
            return string.format("before %s target %s after %s: moved %.2f m (expected ~5)", fmt(before), fmt(target),
                fmt(after), moved)
        end)
    end
    return count .. " vehicle(s) told to move 5 m forward; result in 1 s"
end

-- Can an NPC be seated as a passenger through an AI mount command?
probes.s1vSeatNpc = function()
    local vehicles = probeVehicles()
    local npcs = probeEntities()
    if #vehicles == 0 or #npcs == 0 then
        return "spawn a probe vehicle and a probe NPC first"
    end
    local vehicle = vehicles[1]
    local npc = npcs[1]

    local mountData = NewObject("handle:gameMountEventData")
    mountData.mountParentEntityId = vehicle:GetEntityID()
    mountData.slotName = CName.new("seat_front_right")
    mountData.isInstant = true
    mountData.ignoreHLS = true
    local command = NewObject("handle:AIMountCommand")
    command.mountData = mountData
    npc:GetAIControllerComponent():SendCommand(command)

    later(2.0, "S1v seat result", function()
        local distance = Vector4.Distance(npc:GetWorldPosition(), vehicle:GetWorldPosition())
        return string.format("NPC is %.2f m from the vehicle centre (seated is ~1 m or less)", distance)
    end)
    return "mount command sent; result in 2 s"
end

probes.s1vDumpTypes = function()
    local out = {}
    for _, typeName in ipairs({ "vehicleBaseObject", "vehicleCarBaseObject", "AIMountCommand", "gameMountEventData",
        "gameMountingFacility", "vehicleController", "VehicleComponent" }) do
        local ok, text = pcall(function() return DumpType(typeName, true) end)
        table.insert(out, "==== " .. typeName .. "\n" .. tostring(ok and text or ("error: " .. tostring(text))))
    end
    writeFile("probe-s1v-types.txt", table.concat(out, "\n\n"))
end

probes.cleanup = function()
    entitySystem():DeleteTagged(CName.new("CoopProbe"))
    entitySystem():DeleteTagged(CName.new("CoopProbeVehicle"))
end

-- ---------------------------------------------------------------------------------------------------
-- UI

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
        panel.applyTime = ImGui.Checkbox("Apply to the game (experimental, S8)", panel.applyTime)
    end

    local b = bridge()
    if b then
        ImGui.Text("Puppets spawned: " .. tostring(b:GetPuppetCount()))
        local ok, debugText = pcall(function() return b:GetPuppetDebug() end)
        if ok and debugText and debugText ~= "" then
            ImGui.TextWrapped(debugText)
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
    ImGui.TextWrapped("Results go to probe-results.txt (and probe-*.txt dumps) in this mod's folder.")
    if ImGui.CollapsingHeader("S1 - puppet base record and appearance") then
        probeButton("List candidate records", "s1ListRecords")
        panel.probeRecord = ImGui.InputText("Record", panel.probeRecord, 128)
        probeButton("Spawn record in front of V", "s1Spawn")
        probeButton("Dump customization types", "s1DumpTypes")
    end
    if ImGui.CollapsingHeader("S1 - vehicles") then
        panel.vehicleRecord = ImGui.InputText("Vehicle record", panel.vehicleRecord, 128)
        probeButton("Spawn vehicle in front of V", "s1vSpawn")
        probeButton("Move probe vehicle 5 m (teleport)", "s1vTeleport")
        ImGui.TextWrapped("Also spawn a probe NPC (S1 above), then:")
        probeButton("Seat probe NPC as passenger", "s1vSeatNpc")
        probeButton("Dump vehicle types", "s1vDumpTypes")
    end
    if ImGui.CollapsingHeader("S2 - puppets as AI targets") then
        probeButton("Make probe NPCs player-aligned", "s2MakeAllies")
        ImGui.TextWrapped("Then start a fight nearby: do enemies shoot the probe NPC?")
    end
    if ImGui.CollapsingHeader("S3 - driving an NPC") then
        probeButton("AI-walk probe NPCs to V", "s3WalkToPlayer")
        probeButton("Dump AI and animation types", "s3DumpTypes")
    end
    if ImGui.CollapsingHeader("S8 - time dilation") then
        probeButton("Dump time types", "s8DumpTypes")
        probeButton("World 0.25 for 3 s, V exempt", "s8WorldSlowmo")
        probeButton("Probe NPCs individual 2.0 for 3 s", "s8IndividualFast")
    end
    ImGui.Separator()
    probeButton("Delete probe NPCs", "cleanup")
end

registerForEvent("onInit", function()
    log("coop-dev loaded")
end)

registerForEvent("onOverlayOpen", function() panel.overlayOpen = true end)
registerForEvent("onOverlayClose", function() panel.overlayOpen = false end)

registerForEvent("onUpdate", function()
    if panel.applyTime or panel.timeApplied then
        local ok, system = pcall(function() return Game.GetCoopSystem() end)
        if ok and system then
            if panel.applyTime then
                applyTimeFields(system)
            elseif panel.timeApplied then
                -- Switched off while slowed: restore normal time.
                pcall(function()
                    Game.GetTimeSystem():UnsetTimeDilation(CName.new("coopField"), CName.new(""))
                    Game.GetTimeSystem():SetIgnoreTimeDilationOnLocalPlayerZero(false)
                end)
                panel.timeApplied = false
            end
        end
    end

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
        try("unset slow-mo", function()
            local timeSystem = Game.GetTimeSystem()
            timeSystem:UnsetTimeDilation(CName.new("coopProbe"), CName.new(""))
            timeSystem:SetIgnoreTimeDilationOnLocalPlayerZero(false)
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
