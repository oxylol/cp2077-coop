-- Cp2077Coop dev panel (Cyber Engine Tweaks). Development only; players never need this.
--
-- * Session: host / join / leave / impairment, live status, the puppets' state.
-- * Direct drive: the mirror test (a third-person V next to you, placed every frame and animated with YOUR
--   animation inputs), recording your animation inputs, listing the game's animation functions, and trying
--   graph input names for the motion values live. Test steps: docs/07-testing-guide.md.
-- * Probes: the experiments that are still open (looks, passive puppets, combat, vehicles).
--
-- Every engine call that isn't confirmed yet is wrapped in pcall, so a wrong guess prints an error instead of
-- breaking anything, and writes a line to probe-results.txt first, so after a crash the last line names the call.
-- Please send probe-results.txt (in this mod's folder) back with every test.

local panel = {
    overlayOpen = false,
    address = "127.0.0.1:27077",
    password = "",
    port = 27077,
    impairment = 0,
    lastActivation = 0,
    lines = {},
    checks = {},           -- delayed checks: { at = os.clock() time, label, fn }
    probeRecord = "",      -- filled with the lookalike matching V's body on first draw
    itemRecord = "Items.Preset_Lexington_Default",
    vehicleRecord = "Vehicle.v_standard2_archer_hella_player",
    -- Mirror test
    mirror = nil,          -- { id, startedAt, found, aiOff, frames, following, lagging, lag, lastTarget }
    mirrorAhead = 3.0,
    mirrorSide = 0.0,
    mirrorAnim = true,
    mirrorMethod = 2,      -- placement method (CoopSystem.PlaceEntity): 1 teleport, 2 transform, 3 AI teleport
    mirrorBody = 0,        -- index into mirrorBodies() (0-based for ImGui)
    applyVia = 0,          -- 0 events, 1 controller
    testInput = "crouch",
    testValue = 1.0,
    switchOff = nil,       -- text field for CoopSystem.SetSwitchOffComponents (filled from the plugin)
    ptest = nil,           -- the automatic placement test while it runs
    -- The plugin's defaults since round I (the names V's graph and the lookalike's use); "-name" negates.
    motion = { speed = "speed_horizontal+desired_speed_horizontal", direction = "move_direction", vertical = "speed_vertical",
        turn = "rotation_speed_yaw", moving = "" },
    addImpostor = false,   -- body setup for bodies spawned afterwards (CoopSystem.SetBodyOptions)
    mirrorDress = true,    -- put your V's slot items on the mirror when it appears (round K)
    mirrorTpp = false,     -- switch the mirror's TPP representation on when it appears (round M: no effect)
    dressItem = "Items.PlayerMaTppHead",
    dressSlot = "AttachmentSlots.TppHead",
    slots = nil,           -- attachment slot records, listed once
    -- Raw placement probe and vehicle drive (one probe at a time)
    drive = nil,
}

local impairments = { "none", "lan", "good", "typical", "bad", "awful" }
-- Keep equal to kVersionString in src/core/Version.hpp; the Direct drive tab warns when the plugin differs
-- (an old build still installed).
local PANEL_VERSION = "0.5.10-m1v"
local atan2 = math.atan2 or math.atan

-- ---------------------------------------------------------------------------------------------------
-- Logging and small helpers

local function log(text)
    local line = os.date("%H:%M:%S") .. "  " .. tostring(text)
    print("[coop-dev] " .. tostring(text))
    table.insert(panel.lines, line)
    if #panel.lines > 80 then
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

-- Yaw of a forward vector in the game's convention (0 = +Y, counterclockwise positive), as the bridge sends it.
local function yawOf(forward)
    return math.deg(atan2(-forward.x, forward.y))
end

local function localGender()
    local gender = "Male"
    pcall(function()
        if nameOf(Game.GetPlayer():GetResolvedGenderName()) == "Female" then
            gender = "Female"
        end
    end)
    return gender
end

-- The third-person V record the mod uses for puppets: only the one matching your V's body spawns (round C).
local function lookalikeRecord()
    return "Character.TPP_Player_Cutscene_" .. localGender()
end

-- Bodies the mirror can be. Round I: the PlayerPuppet bodies (TPP_Player, photo mode, replacer) use V's own
-- animation graph and animate with V's inputs but show only a neck; the cutscene lookalike looks like V but its
-- graph doesn't take them. The body setup (impostor, V's graph) tries to get both.
local function mirrorBodies()
    local gender = localGender()
    local short = gender == "Female" and "wa" or "ma"
    return {
        -- Round N: the player's own third-person templates (Character.TPP_Player spawns the female one).
        "template:base\\characters\\entities\\player\\player_" .. short .. "_tpp.ent",
        "template:base\\characters\\entities\\player\\player_" .. short .. "_tpp_reflexion.ent",
        "Character.TPP_Player",
        "Character.TPP_Player_Cutscene_" .. gender,
        "Character.Player_Puppet_Photomode",
        "Character.Player_Replacer_Puppet_Base",
        "Character.TPP_Player_Cutscene_No_Impostor_" .. gender,
        "Cp2077Coop.Character.RemotePlayer",
    }
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
-- it, otherwise through the script bridge's pass-through methods (no animation tools then).
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

-- True if the plugin object has this function (an older plugin build, or the bridge fallback, may not).
local function has(object, name)
    if not object then
        return false
    end
    local ok, value = pcall(function() return object[name] end)
    return ok and value ~= nil
end

-- ---------------------------------------------------------------------------------------------------
-- Note on engine names: in CET an empty string passed to CName.new is NOT the engine's empty name (only
-- "None" is), so passing "" as an ease curve makes the engine look up a curve that doesn't exist; that crashed
-- the game in the first S8 run. Optional curves are left out, or passed as CName.new("None").

-- ---------------------------------------------------------------------------------------------------
-- Mirror test: a third-person V placed every frame in front of (or next to) you, facing where you face, with
-- its AI switched off by the plugin, and YOUR captured animation inputs applied to it. On one game this shows
-- what a remote player's puppet will do with the same inputs: if the mirror walks, runs, crouches and jumps
-- with you, puppets will too.

local placeMethods = { "teleport", "transform", "AI teleport" }

local function methodNeedsAI(method)
    return method == 3
end

local function mirrorEntity()
    local m = panel.mirror
    if not m then
        return nil
    end
    local ok, entity = pcall(function() return entitySystem():GetEntity(m.id) end)
    if ok then
        return entity
    end
    return nil
end

-- Horizontal direction V faces, normalized.
local function flatForward()
    local forward = Game.GetPlayer():GetWorldForward()
    local length = math.sqrt(forward.x * forward.x + forward.y * forward.y)
    if length < 0.001 then
        return { x = 0.0, y = 1.0 }
    end
    return { x = forward.x / length, y = forward.y / length }
end

-- Where the mirror goes: offset from V along a FIXED direction (the anchor, taken when the mirror starts), so
-- turning the camera doesn't swing the mirror around V (which would look like sideways running to its graph).
-- It faces wherever V faces.
local function mirrorTarget(anchor)
    local position = Game.GetPlayer():GetWorldPosition()
    local forward = flatForward()
    local dir = anchor or forward
    local right = { x = dir.y, y = -dir.x }
    local target = Vector4.new(position.x + dir.x * panel.mirrorAhead + right.x * panel.mirrorSide,
        position.y + dir.y * panel.mirrorAhead + right.y * panel.mirrorSide, position.z, 1.0)
    return target, yawOf(forward)
end

local function startMirror()
    if panel.mirror then
        return "a mirror is already running; stop it first"
    end
    local record = mirrorBodies()[panel.mirrorBody + 1] or lookalikeRecord()
    local anchor = flatForward()
    local target = mirrorTarget(anchor)
    local spec = DynamicEntitySpec.new()
    local templatePath = string.match(record, "^template:(.+)$")
    if templatePath then
        local system = coopSystem()
        if not has(system, "SetSpawnTemplate") then
            return "the plugin's SetSpawnTemplate is not reachable (old plugin build?)"
        end
        if not system:SetSpawnTemplate(spec, templatePath) then
            return "SetSpawnTemplate failed for " .. templatePath .. " (see red4ext/logs)"
        end
    else
        spec.recordID = TweakDBID.new(record)
    end
    spec.position = target
    spec.orientation = Game.GetPlayer():GetWorldOrientation()
    spec.alwaysSpawned = true
    spec.tags = { CName.new("CoopMirror") }
    log("mirror: CreateEntity(" .. record .. ")")
    local id = entitySystem():CreateEntity(spec)
    panel.mirror = { id = id, record = record, anchor = anchor, startedAt = os.clock(), found = false, aiOff = nil,
        frames = 0, following = 0, lagging = 0, lag = 0.0, lastTarget = nil, animOn = false, lostAt = nil }
    return "spawning " .. record .. "; it is placed every frame once it appears"
end

local function setMirrorAnim(entity, on)
    local system = coopSystem()
    if not has(system, "MirrorAnimationsTo") then
        log("mirror: the plugin's MirrorAnimationsTo is not reachable from CET")
        return
    end
    if on then
        log("mirror: MirrorAnimationsTo(mirror)")
        system:MirrorAnimationsTo(entity)
    else
        log("mirror: MirrorAnimationsTo(nil)")
        system:MirrorAnimationsTo(nil)
    end
    if panel.mirror then
        panel.mirror.animOn = on
    end
end

local function stopMirror()
    local m = panel.mirror
    if not m then
        return "no mirror running"
    end
    pcall(function() setMirrorAnim(nil, false) end)
    panel.mirror = nil
    entitySystem():DeleteTagged(CName.new("CoopMirror"))
    return string.format("stopped after %d placements (%d following, %d lagging)", m.frames, m.following, m.lagging)
end

local function forgetMirror(reason)
    log("mirror: " .. reason .. "; mirror stopped")
    pcall(function() setMirrorAnim(nil, false) end)
    panel.mirror = nil
end

local dressLikeMe -- the looks tools below (defined after the mirror code, used when the mirror appears)

-- The player body's third-person switch (round M). A spawned player body behaves as if it were first person: it
-- swaps the third-person head for the first-person one (no head, no headgear), its upper body moves like the
-- first-person arms, and its character customization (face, hair, body) isn't applied. The game switches the real
-- V to a full third-person body for some scenes with these events to its TPP representation component.
local tppEvents = { "gamePrepareTPPRepresentationEvent", "gameFinalizeActivationTPPRepresentationEvent" }

local function queueEvent(entity, className)
    log("tpp: QueueEvent(" .. className .. ")")
    local event = NewObject(className)
    entity:QueueEvent(event)
    return className .. " queued"
end

local function switchToTpp()
    local entity = mirrorEntity()
    if not entity then
        return "start the mirror first"
    end
    queueEvent(entity, tppEvents[1])
    later(0.6, "tpp finalize", function()
        local now = mirrorEntity()
        return now and queueEvent(now, tppEvents[2]) or "the mirror is gone"
    end)
    later(2.5, "tpp check", function()
        local now = mirrorEntity()
        if not now then
            return "the mirror is gone"
        end
        local head = Game.GetTransactionSystem():GetItemInSlot(now, TweakDBID.new("AttachmentSlots.TppHead"))
        return "TppHead holds " .. (head and tdbName(head:GetItemID().id) or "nothing") .. "; appearance "
            .. nameOf(now:GetCurrentAppearanceName())
    end)
    return "third-person switch sent"
end

local function updateMirror()
    local m = panel.mirror
    if not m then
        return
    end
    if not Game.GetPlayer() then
        forgetMirror("no V (loading or main menu)")
        return
    end
    local entity = mirrorEntity()
    if not entity then
        if m.found then
            -- Gone after it was there (a save was loaded, or it was deleted): stop after 3 s.
            m.lostAt = m.lostAt or os.clock()
            if os.clock() - m.lostAt > 3.0 then
                forgetMirror("the body is gone")
            end
        elseif os.clock() - m.startedAt > 30.0 and not m.warned then
            m.warned = true
            log("mirror: the body didn't appear within 30 s (" .. m.record .. ")")
        end
        return
    end
    m.lostAt = nil

    local system = coopSystem()
    if not has(system, "PlaceEntity") then
        forgetMirror("the plugin's PlaceEntity is not reachable from CET (old plugin build?)")
        return
    end
    -- AI off for methods that place the body directly, on for the AI teleport.
    local wantAI = methodNeedsAI(panel.mirrorMethod)
    if m.aiOn ~= wantAI and m.found then
        log("mirror: SetPuppetAI(mirror, " .. tostring(wantAI) .. ")")
        local ok, result = pcall(function() return system:SetPuppetAI(entity, wantAI) end)
        m.aiOn = wantAI
        m.aiOff = (not wantAI) and ok and result == true
        log("mirror: AI " .. (wantAI and "on" or "off") .. ": " .. tostring(ok and result) .. (ok and "" or (" (" .. tostring(result) .. ")")))
    end

    if not m.found then
        m.found = true
        m.aiOn = true -- a fresh body has its AI on; switched above on the next frame if needed
        log(string.format("mirror: body appeared after %.1f s", os.clock() - m.startedAt))
        if panel.mirrorAnim then
            local ok, err = pcall(function() setMirrorAnim(entity, true) end)
            if not ok then
                log("mirror: MirrorAnimationsTo FAILED -> " .. tostring(err))
            end
        end
        if panel.mirrorTpp then
            later(0.2, "switch the mirror to third person", switchToTpp)
        end
        if panel.mirrorDress then
            -- A moment later, so the body is fully set up (and switched) before items go on it.
            later(panel.mirrorTpp and 1.2 or 0.5, "dress the mirror like you", function()
                return dressLikeMe(mirrorEntity())
            end)
        end
    end

    -- Did the last placement take? Measured before placing again, and only counted while you move (standing
    -- still, a body that never moves would look like it follows).
    local target, yaw = mirrorTarget(m.anchor)
    if m.lastTarget then
        local okLag, lag = pcall(function() return Vector4.Distance(entity:GetWorldPosition(), m.lastTarget) end)
        if okLag then
            m.lag = lag
            if Vector4.Distance(target, m.lastTarget) > 0.02 then
                if lag < 0.5 then
                    m.following = m.following + 1
                else
                    m.lagging = m.lagging + 1
                end
            end
        end
    end

    -- AI teleports go through the AI's command queue: at most ten a second.
    if methodNeedsAI(panel.mirrorMethod) and m.lastPlace and os.clock() - m.lastPlace < 0.1 then
        return
    end
    local ok, placed = pcall(function() return system:PlaceEntity(entity, target, yaw, panel.mirrorMethod) end)
    m.lastPlace = os.clock()
    m.lastTarget = target
    m.frames = m.frames + 1
    if not ok or not placed then
        local reason = ok and tostring(system:GetPlacementError()) or tostring(placed)
        if reason ~= m.lastError then
            m.lastError = reason
            log("mirror: placing with " .. placeMethods[panel.mirrorMethod] .. " FAILED -> " .. reason)
        end
    end
end

local function mirrorStatus()
    local m = panel.mirror
    if not m then
        return "Mirror: off"
    end
    if not m.found then
        return string.format("Mirror: %s spawning for %.0f s", m.record, os.clock() - m.startedAt)
    end
    local total = m.following + m.lagging
    local share = total > 0 and string.format("%.0f%%", 100.0 * m.following / total) or "(move to measure)"
    return string.format("Mirror (%s): %d placements; while you moved it followed %s of frames; now %.2f m off; AI %s; your inputs applied: %s",
        placeMethods[panel.mirrorMethod], m.frames, share, m.lag, m.aiOn and "on" or "off", tostring(m.animOn))
end

-- Dumps the animation, movement and AI components of an entity (to compare the mirror with V, and to find the
-- animation graph a body uses).
local animComponent = { "Anim", "Move", "AI", "Locomotion", "Character", "Skinned", "Impostor" }

local function dumpAnimComponents(out, label, entity)
    table.insert(out, "######## " .. label)
    local ok, components = pcall(function() return entity:GetComponents() end)
    if not ok or not components then
        table.insert(out, "GetComponents failed: " .. tostring(components))
        return
    end
    for _, component in ipairs(components) do
        local className = nameOf(component:GetClassName())
        for _, pattern in ipairs(animComponent) do
            if string.find(className, pattern) then
                local okDump, text = pcall(function() return Dump(component, true) end)
                table.insert(out, "==== " .. className .. " " .. nameOf(component:GetName()) .. "\n"
                    .. tostring(okDump and text or ("error: " .. tostring(text))))
                break
            end
        end
    end
end

local function dumpMirrorComponents()
    local out = {}
    local entity = mirrorEntity()
    if entity then
        dumpAnimComponents(out, "mirror (" .. panel.mirror.record .. ")", entity)
    end
    dumpAnimComponents(out, "your V", Game.GetPlayer())
    writeFile("probe-anim-components.txt", table.concat(out, "\n"))
    return entity and "mirror and V dumped" or "V dumped (no mirror running)"
end

-- What the animation graphs of V and of the mirror accept (variables and AnimFeature slots), written by the plugin
-- to red4ext/plugins/Cp2077Coop/anim-graphs-*.txt.
local function dumpAnimGraphs()
    local system = coopSystem()
    if not has(system, "DumpAnimGraphs") then
        return "the plugin's DumpAnimGraphs is not reachable from CET (old plugin build?)"
    end
    log("graphs: " .. tostring(system:DumpAnimGraphs(Game.GetPlayer(), "V")))
    local entity = mirrorEntity()
    if entity then
        local label = "mirror-" .. string.gsub(panel.mirror.record, "[^%w]", "_")
        log("graphs: " .. tostring(system:DumpAnimGraphs(entity, label)))
        return "V and mirror written"
    end
    return "V written (start the mirror to get its graphs too)"
end

local function applyTest(name, value)
    local system = coopSystem()
    local entity = mirrorEntity()
    if not entity or not has(system, "ApplyAnimTest") then
        return "start the mirror first"
    end
    return system:ApplyAnimTest(entity, name, value) .. " input(s) applied: " .. name
end

-- ---------------------------------------------------------------------------------------------------
-- Looks: on V, the body, head, hair and clothes are components that come with items in attachment slots (the
-- round I dump of V has them as garment and morph-target meshes); a spawned player body (TPP_Player) has none of
-- those items, which is why it is only a neck (rounds I and J). Listing every slot of V and the mirror, and
-- dressing the mirror with V's items, the way the round C probe put an item on the lookalike.

-- Slots everyone knows, used if TweakDB can't list the slot records.
local knownSlots = { "TppHead", "Head", "Face", "Eyes", "Chest", "Torso", "Legs", "Feet", "Outfit", "UnderwearTop",
    "UnderwearBottom", "WeaponRight", "WeaponLeft" }

local function attachmentSlots()
    if panel.slots then
        return panel.slots
    end
    local slots = {}
    local ok, records = pcall(function() return TweakDB:GetRecords("gamedataAttachmentSlot_Record") end)
    if ok and type(records) == "table" and #records > 0 then
        for _, record in ipairs(records) do
            local okID, id = pcall(function() return record:GetID() end)
            if okID and id then
                table.insert(slots, { id = id, name = tdbName(id) })
            end
        end
    else
        log("looks: TweakDB slot records not listed (" .. tostring(records) .. "); using the known slots")
        for _, name in ipairs(knownSlots) do
            table.insert(slots, { id = TweakDBID.new("AttachmentSlots." .. name), name = "AttachmentSlots." .. name })
        end
    end
    table.sort(slots, function(a, b) return a.name < b.name end)
    panel.slots = slots
    return slots
end

-- { slot = { id, name }, itemID, name } for every slot of the entity that holds an item.
local function itemsInSlots(entity)
    local transactions = Game.GetTransactionSystem()
    local found = {}
    for _, slot in ipairs(attachmentSlots()) do
        local ok, item = pcall(function() return transactions:GetItemInSlot(entity, slot.id) end)
        if ok and item then
            local okID, itemID = pcall(function() return item:GetItemID() end)
            if okID and itemID then
                table.insert(found, { slot = slot, itemID = itemID, name = tdbName(itemID.id) })
            end
        end
    end
    return found
end

local function listComponents(out, label, entity)
    table.insert(out, "######## components of " .. label)
    local ok, components = pcall(function() return entity:GetComponents() end)
    if not ok or not components then
        table.insert(out, "GetComponents failed: " .. tostring(components))
        return
    end
    for _, component in ipairs(components) do
        local className = nameOf(component:GetClassName())
        local okOn, on = pcall(function() return component:IsEnabled() end)
        table.insert(out, className .. "  " .. nameOf(component:GetName())
            .. ((okOn and on == false) and "  (off)" or ""))
    end
end

local function listSlots(out, label, entity)
    local items = itemsInSlots(entity)
    table.insert(out, "######## " .. #items .. " item(s) in the slots of " .. label)
    for _, entry in ipairs(items) do
        table.insert(out, entry.slot.name .. ": " .. entry.name)
    end
    return #items
end

-- Writes probe-looks.txt: V's and the mirror's slot items and components.
local function listLooks()
    local out = {}
    local slotCount = #attachmentSlots()
    table.insert(out, slotCount .. " attachment slot(s) checked")
    local function describe(label, object)
        field(out, label .. " appearance", function() return nameOf(object:GetCurrentAppearanceName()) end)
        field(out, label .. " template", function() return tostring(object:GetTemplatePath()) end)
    end
    describe("your V", Game.GetPlayer())
    if mirrorEntity() then
        describe("the mirror", mirrorEntity())
    end
    local vCount = listSlots(out, "your V", Game.GetPlayer())
    local entity = mirrorEntity()
    local mCount = entity and listSlots(out, "the mirror (" .. panel.mirror.record .. ")", entity) or 0
    listComponents(out, "your V", Game.GetPlayer())
    if entity then
        listComponents(out, "the mirror (" .. panel.mirror.record .. ")", entity)
    end
    writeFile("probe-looks.txt", table.concat(out, "\n"))
    return string.format("%d slots checked; V has %d item(s) in slots, the mirror %s", slotCount, vCount,
        entity and tostring(mCount) or "(not running)")
end

local function tppHeadRecord()
    return localGender() == "Female" and "Items.PlayerWaTppHead" or "Items.PlayerMaTppHead"
end

-- Gives the mirror every item V has in a slot (weapons left out; V's first-person head swapped for the
-- third-person one; other first-person items left out) and puts each in the same slot.
dressLikeMe = function(entity)
    if not entity then
        return "start the mirror first"
    end
    local okImpostor, impostor = pcall(function() return entity:FindComponentByType(CName.new("gameImpostorComponent")) end)
    if okImpostor and impostor then
        return "skipped: it copies your V through its impostor"
    end
    local transactions = Game.GetTransactionSystem()
    local dressed, failed, skipped = 0, 0, 0
    local hadHead = false
    local function put(slotID, slotName, itemID, itemName)
        log("dress: GiveItem + AddItemToSlot(" .. slotName .. ", " .. itemName .. ")")
        local ok, placed = pcall(function()
            transactions:GiveItem(entity, itemID, 1)
            return transactions:AddItemToSlot(entity, slotID, itemID)
        end)
        if ok and placed then
            dressed = dressed + 1
        else
            failed = failed + 1
            log("dress: " .. slotName .. " FAILED -> " .. tostring(placed))
        end
    end
    for _, entry in ipairs(itemsInSlots(Game.GetPlayer())) do
        local slotName, itemName = entry.slot.name, entry.name
        if string.find(slotName, "Weapon") then
            skipped = skipped + 1
        elseif string.find(itemName, "FppHead") then
            hadHead = true
            local head = tppHeadRecord()
            put(entry.slot.id, slotName, ItemID.FromTDBID(TweakDBID.new(head)), head)
        elseif string.find(itemName, "Fpp") then
            skipped = skipped + 1
            log("dress: skipped first-person item " .. itemName .. " (" .. slotName .. ")")
        else
            if string.find(itemName, "TppHead") then
                hadHead = true
            end
            put(entry.slot.id, slotName, entry.itemID, itemName)
        end
    end
    if not hadHead then
        local head = tppHeadRecord()
        put(TweakDBID.new("AttachmentSlots.TppHead"), "AttachmentSlots.TppHead (added)",
            ItemID.FromTDBID(TweakDBID.new(head)), head)
    end
    later(2.0, "dress check", function()
        local now = mirrorEntity()
        return now and (#itemsInSlots(now) .. " item(s) in the mirror's slots now") or "the mirror is gone"
    end)
    return string.format("%d item(s) put on, %d failed, %d skipped (weapons, first-person)", dressed, failed,
        skipped)
end

-- One item (a TweakDB record name) into one slot of the mirror, for trying what the dressing leaves out.
local function putOnMirror(itemRecord, slotName)
    local entity = mirrorEntity()
    if not entity then
        return "start the mirror first"
    end
    local transactions = Game.GetTransactionSystem()
    local itemID = ItemID.FromTDBID(TweakDBID.new(itemRecord))
    local slot = TweakDBID.new(slotName)
    log("dress: GiveItem + AddItemToSlot(" .. slotName .. ", " .. itemRecord .. ")")
    transactions:GiveItem(entity, itemID, 1)
    local placed = transactions:AddItemToSlot(entity, slot, itemID)
    later(1.0, "item check", function()
        local now = mirrorEntity()
        local item = now and transactions:GetItemInSlot(now, slot)
        return slotName .. " on the mirror holds " .. (item and tdbName(item:GetItemID().id) or "nothing")
    end)
    return "AddItemToSlot returned " .. tostring(placed)
end

-- ---------------------------------------------------------------------------------------------------
-- Probes that are still open

local function spawnInFront(recordName, distance, tag)
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
    spec.tags = { CName.new(tag or "CoopProbe") }
    return entitySystem():CreateEntity(spec)
end

local function probeEntities()
    return entitySystem():GetTagged(CName.new("CoopProbe"))
end

local function probeVehicles()
    return entitySystem():GetTagged(CName.new("CoopProbeVehicle"))
end

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

local probes = {}

probes.spawn = function()
    return spawnInFront(panel.probeRecord, 3.0)
end

probes.listRecords = function()
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
    writeFile("probe-records.txt", table.concat(found, "\n"))
    return #found .. " candidate records"
end

-- S1c: per-player looks. The impostor component copies the local V onto a lookalike; slotIDsToOmit looks like a
-- list of slots it does NOT copy.
probes.impostorValues = function()
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

probes.impostorOmitWeapons = function()
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

probes.putItem = function()
    local transactions = Game.GetTransactionSystem()
    local itemID = ItemID.FromTDBID(TweakDBID.new(panel.itemRecord))
    local slot = TweakDBID.new("AttachmentSlots.WeaponRight")
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        log("S1c: GiveItem(" .. panel.itemRecord .. ") and AddItemToSlot(WeaponRight) on a probe")
        transactions:GiveItem(entity, itemID, 1)
        transactions:AddItemToSlot(entity, slot, itemID)
        count = count + 1
    end
    return count .. " probe(s): does it hold the item, and does it still copy V's weapon?"
end

-- S2d: puppets must not fight on their own (round D: a plain-NPC puppet joined a fight by itself).
probes.passive = function()
    local count = 0
    for _, entity in ipairs(probeEntities()) do
        log("S2d: EnableSensesComponent(false) on a probe")
        entity:EnableSensesComponent(false)
        count = count + 1
    end
    return count .. " probe(s) with senses off: Diagnose (can it still be seen?), then get into a fight next to it"
end

-- S2b/S2c: will enemies attack a puppet? Crosshair on an enemy first.
probes.diagnose = function()
    local player = Game.GetPlayer()
    local npc = lookAtObject()
    local out = { "---- " .. os.date("%H:%M:%S") }
    field(out, "looking at", function()
        if npc == nil then
            return "nothing"
        end
        return nameOf(npc:GetClassName()) .. " " .. tdbName(npc:GetRecordID())
    end)
    if npc then
        field(out, "NPC attitude towards V", function() return tostring(npc:GetAttitudeTowards(player)) end)
        field(out, "NPC in combat", function() return npc:IsInCombat() end)
    end
    for i, probe in ipairs(probeEntities()) do
        local p = "probe " .. i .. " "
        field(out, p .. "class", function() return nameOf(probe:GetClassName()) .. " " .. tdbName(probe:GetRecordID()) end)
        field(out, p .. "attitude group", function() return nameOf(probe:GetAttitudeAgent():GetAttitudeGroup()) end)
        field(out, p .. "can be seen (visible object)", function() return probe:GetVisibleObjectComponent() ~= nil end)
        field(out, p .. "has senses", function() return probe:GetSensesComponent() ~= nil end)
        if npc then
            field(out, p .. "NPC attitude towards it", function() return tostring(npc:GetAttitudeTowards(probe)) end)
        end
    end
    for _, line in ipairs(out) do
        log("diagnose " .. line)
    end
    return "see the lines above"
end

probes.hostile = function()
    local npc = lookAtObject()
    if npc == nil then
        return "put the crosshair on an NPC first"
    end
    local hostile = Enum.new("EAIAttitude", "AIA_Hostile")
    local count = 0
    for _, probe in ipairs(probeEntities()) do
        try("NPC hostile towards probe", function()
            npc:GetAttitudeAgent():SetAttitudeTowards(probe:GetAttitudeAgent(), hostile)
            return tostring(npc:GetAttitudeTowards(probe))
        end)
        try("probe hostile towards NPC", function()
            probe:GetAttitudeAgent():SetAttitudeTowards(npc:GetAttitudeAgent(), hostile)
            return tostring(probe:GetAttitudeTowards(npc))
        end)
        count = count + 1
    end
    return count .. " probe(s); does the NPC go after it now?"
end

probes.threat = function()
    local npc = lookAtObject()
    if npc == nil then
        return "put the crosshair on an NPC first"
    end
    local count = 0
    for _, probe in ipairs(probeEntities()) do
        try("TargetTrackingExtension.InjectThreat(npc, probe)", function()
            TargetTrackingExtension.InjectThreat(npc, probe)
        end)
        try("target tracker AddThreat(probe)", function()
            return npc:GetTargetTrackerComponent():AddThreat(probe, true, probe:GetWorldPosition(), 1.0, 30.0, false)
        end)
        count = count + 1
    end
    return count .. " probe(s); does the NPC shoot at it now?"
end

-- Vehicles (S1v: all confirmed; kept for the in-game vehicle work).
probes.vehicleSpawn = function()
    return spawnInFront(panel.vehicleRecord, 8.0, "CoopProbeVehicle")
end

local function seatProbe(slotName)
    local vehicles = probeVehicles()
    local npcs = probeEntities()
    if #vehicles == 0 or #npcs == 0 then
        return "spawn a probe vehicle and a probe first"
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
    later(2.0, "seat result (" .. slotName .. ")", function()
        local distance = Vector4.Distance(npc:GetWorldPosition(), vehicle:GetWorldPosition())
        return string.format("probe is %.2f m from the vehicle centre (seated is ~1 m or less)", distance)
    end)
    return "mount command sent; result in 2 s"
end

probes.seatDriver = function() return seatProbe("seat_front_left") end
probes.seatPassenger = function() return seatProbe("seat_front_right") end

probes.vehicleDrive = function()
    local vehicles = probeVehicles()
    if #vehicles == 0 then
        return "spawn a probe vehicle first"
    end
    local vehicle = vehicles[1]
    local forward = vehicle:GetWorldForward()
    panel.drive = {
        vehicle = vehicle,
        start = vehicle:GetWorldPosition(),
        forward = forward,
        yaw = yawOf(forward),
        began = os.clock(),
        duration = 3.0,
        speed = 5.0,
        frames = 0,
    }
    return "moving the probe vehicle 15 m forward over 3 s, one teleport per frame"
end

local function updateVehicleDrive()
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
        log("vehicle drive failed: " .. tostring(err))
        panel.drive = nil
    elseif elapsed >= d.duration then
        panel.drive = nil
        later(0.5, "vehicle drive result", function()
            local moved = Vector4.Distance(d.start, d.vehicle:GetWorldPosition())
            return string.format("%d teleports, moved %.2f m (expected 15)", d.frames, moved)
        end)
    end
end

probes.dumpTypes = function()
    dumpTypes("probe-types.txt", {
        "entAnimationControllerComponent", "AIHumanComponent", "gameTeleportationFacility", "gameImpostorComponent",
        "AnimFeature_PlayerLocomotionStateMachine", "AnimFeature_Locomotion", "gamePuppet", "PlayerPuppet",
    })
end

probes.cleanup = function()
    panel.drive = nil
    entitySystem():DeleteTagged(CName.new("CoopProbe"))
    entitySystem():DeleteTagged(CName.new("CoopProbeVehicle"))
end

-- ---------------------------------------------------------------------------------------------------
-- Placement test: which way of placing a body actually moves it? (Round F: the teleportation facility moves
-- cars and V, but not a spawned NPC, with or without its AI.) Spawns one third-person V 4 m ahead and tries every
-- method with and without its AI and movement components, 3 m at a time, measuring how far it really went and
-- whether it stayed there. About 20 s; stand still while it runs.

local placementConfigs = {
    { method = 1, off = {}, label = "teleport, nothing off" },
    { method = 1, off = { "AIHumanComponent" }, label = "teleport, AI off" },
    { method = 1, off = { "AIHumanComponent", "moveComponent" }, label = "teleport, AI and movement off" },
    { method = 2, off = {}, label = "transform, nothing off" },
    { method = 2, off = { "AIHumanComponent" }, label = "transform, AI off" },
    { method = 2, off = { "AIHumanComponent", "moveComponent" }, label = "transform, AI and movement off" },
    { method = 3, off = {}, label = "AI teleport (needs the AI on)" },
}

local function testLog(text)
    log("placement test: " .. text)
end

local function startPlacementTest()
    if panel.ptest then
        return "already running"
    end
    local system = coopSystem()
    if not has(system, "PlaceEntity") or not has(system, "SetEntityComponents") then
        return "the plugin's placement functions are not reachable from CET (old plugin build?)"
    end
    local record = lookalikeRecord()
    local forward = flatForward()
    local id = spawnInFront(record, 4.0, "CoopPlacementTest")
    panel.ptest = { id = id, record = record, dir = forward, yaw = yawOf(forward), index = 0, phase = "spawn",
        at = os.clock(), spawnedAt = os.clock(), results = {} }
    testLog("spawned " .. record .. "; stand still")
    return "running (about 20 s); results in the Log tab"
end

local function finishPlacementTest(reason)
    local t = panel.ptest
    panel.ptest = nil
    pcall(function() entitySystem():DeleteTagged(CName.new("CoopPlacementTest")) end)
    if reason then
        testLog(reason)
    end
    if not t then
        return
    end
    local best = nil
    for _, r in ipairs(t.results) do
        testLog(string.format("%-34s call %-5s after 0.2 s %.2f m, after 1 s %.2f m (asked 3.00)%s", r.label,
            tostring(r.called), r.d1 or -1, r.d2 or -1, r.note or ""))
        if not best and r.called and (r.d2 or 0) > 2.5 then
            best = r
        end
    end
    if best then
        testLog("WORKS: " .. best.label .. ". The mirror now uses it; try Start mirror.")
        panel.mirrorMethod = best.method
        local extra = {}
        for _, name in ipairs(best.off) do
            if name ~= "AIHumanComponent" then
                table.insert(extra, name)
            end
        end
        panel.switchOff = table.concat(extra, ",")
        pcall(function() coopSystem():SetSwitchOffComponents(panel.switchOff) end)
    else
        testLog("no method moved the body 2.5 m and kept it there")
    end
end

local function setComponents(system, entity, names, on)
    for _, name in ipairs(names) do
        testLog("SetEntityComponents(" .. name .. ", " .. tostring(on) .. ")")
        local ok, count = pcall(function() return system:SetEntityComponents(entity, name, on) end)
        testLog("  -> " .. tostring(ok and count or ("error: " .. tostring(count))))
    end
end

local function updatePlacementTest()
    local t = panel.ptest
    if not t then
        return
    end
    local now = os.clock()
    if now < t.at then
        return
    end
    local system = coopSystem()
    local entity = nil
    pcall(function() entity = entitySystem():GetEntity(t.id) end)
    if not entity then
        if t.phase == "spawn" and now - t.spawnedAt < 30.0 then
            return
        end
        finishPlacementTest("the test body is gone")
        return
    end

    if t.phase == "spawn" then
        t.phase = "prepare"
        t.at = now + 1.0 -- let it settle
        return
    end

    local config = placementConfigs[t.index]
    if t.phase == "prepare" then
        t.index = t.index + 1
        config = placementConfigs[t.index]
        if not config then
            finishPlacementTest(nil)
            return
        end
        testLog("---- " .. config.label)
        setComponents(system, entity, config.off, false)
        t.phase = "move"
        t.at = now + 0.5
    elseif t.phase == "move" then
        local start = entity:GetWorldPosition()
        local target = Vector4.new(start.x + t.dir.x * 3.0, start.y + t.dir.y * 3.0, start.z, 1.0)
        t.start = start
        t.target = target
        testLog("PlaceEntity(" .. placeMethods[config.method] .. ")")
        local ok, placed = pcall(function() return system:PlaceEntity(entity, target, t.yaw, config.method) end)
        local result = { label = config.label, method = config.method, off = config.off, called = ok and placed }
        if not (ok and placed) then
            result.note = "  [" .. (ok and tostring(system:GetPlacementError()) or tostring(placed)) .. "]"
        end
        testLog("  -> " .. tostring(result.called))
        table.insert(t.results, result)
        t.phase = "check1"
        t.at = now + 0.2
    elseif t.phase == "check1" then
        t.results[#t.results].d1 = Vector4.Distance(entity:GetWorldPosition(), t.start)
        t.phase = "check2"
        t.at = now + 0.8
    elseif t.phase == "check2" then
        local r = t.results[#t.results]
        r.d2 = Vector4.Distance(entity:GetWorldPosition(), t.start)
        if r.d1 and r.d1 > 2.5 and r.d2 < 1.0 then
            r.note = (r.note or "") .. "  [moved, then went back]"
        end
        setComponents(system, entity, config.off, true)
        t.phase = "prepare"
        t.at = now + 0.8
    end
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

local function animStatus(system)
    if not has(system, "GetAnimStatus") then
        return nil
    end
    local ok, text = pcall(function() return system:GetAnimStatus() end)
    if ok then
        return text
    end
    return "GetAnimStatus failed: " .. tostring(text)
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

    -- Time fields (Sandevistan): trigger one for V and watch the rates.
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
    end

    ImGui.Separator()
    local b = bridge()
    if b then
        ImGui.Text("Puppets: " .. tostring(b:GetPuppetCount()))
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
            log("animation: " .. tostring(animStatus(system)))
        end
    elseif not Game.GetPlayer() then
        ImGui.TextWrapped("Load a save: the script bridge starts with the game session.")
    else
        ImGui.TextWrapped("Script bridge not found: redscript did not load r6\\scripts\\Cp2077Coop. "
            .. "Check r6\\logs\\redscript_rCURRENT.log and that Codeware is installed.")
    end

    local anim = animStatus(system)
    if anim and ImGui.CollapsingHeader("Animation inputs") then
        ImGui.TextWrapped(anim)
    end
end

local function drawDirectDrive()
    local system = coopSystem()
    if not has(system, "GetAnimStatus") then
        ImGui.TextWrapped("The plugin's animation tools are not reachable from CET (load a save; check red4ext/logs).")
        return
    end
    local pluginVersion = has(system, "GetVersion") and tostring(system:GetVersion()) or "older than 0.5.10"
    if pluginVersion ~= PANEL_VERSION then
        ImGui.TextWrapped(string.format("VERSION MISMATCH: panel %s, plugin %s. Copy the new cp2077-coop folder over "
            .. "your project folder, run xmake, and restart the game.", PANEL_VERSION, pluginVersion))
    else
        ImGui.Text("Version " .. PANEL_VERSION)
    end

    ImGui.TextWrapped("1. Placement test: which way of placing a body moves it. Stand still while it runs (~20 s); "
        .. "the result is in the Log tab and is picked for the mirror.")
    if panel.ptest then
        ImGui.Text(string.format("Placement test running: %d of %d", math.max(panel.ptest.index, 0), #placementConfigs))
    elseif ImGui.Button("Run placement test") then
        try("placement test", startPlacementTest)
    end

    ImGui.Separator()
    ImGui.TextWrapped("2. Mirror test: a third-person V in front of you, placed every frame with the method below, "
        .. "with YOUR animation inputs applied. It keeps its offset in a fixed direction (turning doesn't swing it "
        .. "around you); 'Put it in front of me again' after you turned. Steps: docs/07-testing-guide.md.")
    local bodyChanged
    panel.mirrorBody, bodyChanged = ImGui.Combo("Body", panel.mirrorBody, mirrorBodies(), #mirrorBodies())
    if bodyChanged then
        log("mirror: body " .. mirrorBodies()[panel.mirrorBody + 1] .. " (applies at the next Start mirror)")
    end
    if not panel.bodySynced and has(system, "GetBodyOptions") then
        -- Start from what the plugin does now (coop.ini, or the last change from this panel).
        local options = tonumber(system:GetBodyOptions()) or 0
        panel.addImpostor = options % 2 == 1
        panel.borrowAnimsets = math.floor(options / 2) % 2 == 1
        panel.bodySynced = true
    end
    if not panel.motionSynced and has(system, "GetMotionInputs") then
        local names = {}
        for name in (tostring(system:GetMotionInputs() or "") .. ","):gmatch("([^,]*),") do
            names[#names + 1] = name
        end
        if #names == 5 then
            panel.motion = { speed = names[1], direction = names[2], vertical = names[3], turn = names[4],
                moving = names[5] }
        end
        panel.motionSynced = true
    end
    local impostorChanged, borrowChanged
    panel.addImpostor, impostorChanged = ImGui.Checkbox("Copy my look onto it (add an impostor)", panel.addImpostor)
    panel.borrowAnimsets, borrowChanged = ImGui.Checkbox("Lend it my animation sets (NPC bodies; may crash)",
        panel.borrowAnimsets or false)
    if (impostorChanged or borrowChanged) and has(system, "SetBodyOptions") then
        try(string.format("body setup: impostor %s, lend animation sets %s (next Start mirror)",
            tostring(panel.addImpostor), tostring(panel.borrowAnimsets)),
            function() system:SetBodyOptions(panel.addImpostor, panel.borrowAnimsets) end)
    end
    panel.mirrorTpp = ImGui.Checkbox("Switch it to third person when it appears", panel.mirrorTpp)
    panel.mirrorDress = ImGui.Checkbox("Dress it in my items when it appears", panel.mirrorDress)
    if ImGui.Button("Switch it to third person now") then
        try("switch the mirror to third person", switchToTpp)
    end
    ImGui.SameLine()
    if ImGui.Button("Dress it like me now") then
        try("dress the mirror like you", function() return dressLikeMe(mirrorEntity()) end)
    end
    ImGui.SameLine()
    if ImGui.Button("List looks (V and mirror)") then
        try("list looks", listLooks)
    end
    panel.dressItem = ImGui.InputText("Item##dress", panel.dressItem, 128)
    panel.dressSlot = ImGui.InputText("Slot##dress", panel.dressSlot, 128)
    if ImGui.Button("Put this item on the mirror") then
        try("put " .. panel.dressItem .. " in " .. panel.dressSlot, function()
            return putOnMirror(panel.dressItem, panel.dressSlot)
        end)
    end
    local methodChanged
    local methodIndex = panel.mirrorMethod - 1
    methodIndex, methodChanged = ImGui.Combo("Placement", methodIndex, placeMethods, #placeMethods)
    if methodChanged then
        panel.mirrorMethod = methodIndex + 1
        log("mirror: placement method " .. placeMethods[panel.mirrorMethod])
    end
    if panel.switchOff == nil and has(system, "GetSwitchOffComponents") then
        panel.switchOff = tostring(system:GetSwitchOffComponents())
    end
    panel.switchOff = ImGui.InputText("Also switch off", panel.switchOff or "", 128)
    if ImGui.Button("Use for mirror and puppets") then
        try("switch off along with the AI: " .. panel.switchOff, function()
            system:SetSwitchOffComponents(panel.switchOff)
        end)
    end
    panel.mirrorAhead = ImGui.DragFloat("Ahead (m)", panel.mirrorAhead, 0.1, -5.0, 10.0)
    panel.mirrorSide = ImGui.DragFloat("Right (m)", panel.mirrorSide, 0.1, -5.0, 5.0)
    if ImGui.Button("Start mirror") then
        try("start mirror", startMirror)
    end
    ImGui.SameLine()
    if ImGui.Button("Stop mirror") then
        try("stop mirror", stopMirror)
    end
    local changed
    panel.mirrorAnim, changed = ImGui.Checkbox("Apply my animation inputs to it", panel.mirrorAnim)
    if changed and panel.mirror and panel.mirror.found then
        try("mirror inputs " .. (panel.mirrorAnim and "on" or "off"), function()
            setMirrorAnim(mirrorEntity(), panel.mirrorAnim)
        end)
    end
    if panel.mirror and ImGui.Button("Put it in front of me again") then
        panel.mirror.anchor = flatForward()
        log("mirror: re-anchored in front of V")
    end
    ImGui.TextWrapped(mirrorStatus())
    if ImGui.Button("Log mirror state") then
        log(mirrorStatus())
    end

    ImGui.Separator()
    if ImGui.Button("Record my animation inputs (10 s)") then
        try("recording (move around now)", function() return system:StartAnimRecording(10.0) end)
    end
    ImGui.SameLine()
    if ImGui.Button("List animation functions") then
        try("list animation functions", function() return system:DumpAnimFunctions() end)
    end
    if ImGui.Button("Dump animation components (mirror and V)") then
        try("dump animation components", dumpMirrorComponents)
    end
    ImGui.SameLine()
    if ImGui.Button("Dump animation graphs (V and mirror)") then
        try("dump animation graphs", dumpAnimGraphs)
    end

    local viaChanged
    panel.applyVia, viaChanged = ImGui.Combo("Apply inputs via", panel.applyVia, { "events", "controller" }, 2)
    if viaChanged then
        local via = panel.applyVia == 1 and "controller" or "events"
        try("apply inputs via " .. via, function() system:SetAnimApplyVia(via) end)
    end
    panel.testInput = ImGui.InputText("Test input (!name = event)", panel.testInput, 64)
    panel.testValue = ImGui.DragFloat("Test value", panel.testValue, 0.1, -10.0, 10.0)
    if ImGui.Button("Apply test input to the mirror") then
        try("test input", function() return applyTest(panel.testInput, panel.testValue) end)
    end
    ImGui.SameLine()
    if ImGui.Button("crouch 1") then
        try("test input", function() return applyTest("crouch", 1.0) end)
    end
    ImGui.SameLine()
    if ImGui.Button("crouch 0") then
        try("test input", function() return applyTest("crouch", 0.0) end)
    end
    ImGui.SameLine()
    if ImGui.Button("!Jump") then
        try("test input", function() return applyTest("!Jump", 0.0) end)
    end

    ImGui.Separator()
    ImGui.TextWrapped("Motion inputs: graph inputs fed with the body's movement (empty = not sent). "
        .. "Type candidate names, press Use, and watch the mirror.")
    panel.motion.speed = ImGui.InputText("Speed (m/s)", panel.motion.speed, 64)
    panel.motion.direction = ImGui.InputText("Direction (deg)", panel.motion.direction, 64)
    panel.motion.vertical = ImGui.InputText("Vertical speed (m/s)", panel.motion.vertical, 64)
    panel.motion.turn = ImGui.InputText("Turn rate (deg/s)", panel.motion.turn, 64)
    panel.motion.moving = ImGui.InputText("Moving (bool)", panel.motion.moving, 64)
    if panel.motionFeature == nil and has(system, "GetMotionFeature") then
        panel.motionFeature = system:GetMotionFeature() == true
    end
    if panel.motionFeature ~= nil then
        local featureChanged
        panel.motionFeature, featureChanged = ImGui.Checkbox("Also send my walking as V's movement feature (playerLocomotion)",
            panel.motionFeature)
        if featureChanged then
            try("walking feature " .. (panel.motionFeature and "on" or "off"), function()
                system:SetMotionFeature(panel.motionFeature)
            end)
        end
    end
    if panel.tppFeature == nil and has(system, "GetTppFeature") then
        panel.tppFeature = system:GetTppFeature() == true
    end
    if panel.tppFeature ~= nil then
        local tppChanged
        panel.tppFeature, tppChanged = ImGui.Checkbox("Also tell it to animate as third person (TPPRepresentation)",
            panel.tppFeature)
        if tppChanged then
            try("third-person feature " .. (panel.tppFeature and "on" or "off"), function()
                system:SetTppFeature(panel.tppFeature)
            end)
        end
    end
    if ImGui.Button("Use these names") then
        local m = panel.motion
        try(string.format("motion inputs speed=%s direction=%s vertical=%s turn=%s moving=%s", m.speed, m.direction,
            m.vertical, m.turn, m.moving), function()
            system:SetMotionInputs(m.speed, m.direction, m.vertical, m.turn, m.moving)
        end)
    end

    ImGui.Separator()
    ImGui.TextWrapped(animStatus(system) or "")
end

local function probeButton(label, key)
    if ImGui.Button(label) then
        try(label, probes[key])
    end
end

local function drawProbes()
    ImGui.TextWrapped("Results go to probe-results.txt (and probe-*.txt files) in this mod's folder.")
    panel.probeRecord = ImGui.InputText("Record", panel.probeRecord, 128)
    probeButton("Spawn record in front of V", "spawn")
    ImGui.SameLine()
    probeButton("Delete probes and cars", "cleanup")
    probeButton("List candidate records", "listRecords")
    ImGui.SameLine()
    probeButton("Dump types", "dumpTypes")

    if ImGui.CollapsingHeader("Looks (S1c): the lookalike copies YOUR V") then
        probeButton("List impostor settings", "impostorValues")
        probeButton("Impostor: don't copy weapons", "impostorOmitWeapons")
        panel.itemRecord = ImGui.InputText("Item", panel.itemRecord, 128)
        probeButton("Put item in the probe's right hand", "putItem")
    end
    if ImGui.CollapsingHeader("Passive puppets (S2d) and combat (S2b)") then
        ImGui.TextWrapped("Crosshair on an enemy for the last three.")
        probeButton("Make probes passive (senses off)", "passive")
        probeButton("Diagnose", "diagnose")
        probeButton("Make that NPC and the probes hostile", "hostile")
        probeButton("Give that NPC the probes as threats", "threat")
    end
    if ImGui.CollapsingHeader("Vehicles") then
        panel.vehicleRecord = ImGui.InputText("Vehicle record", panel.vehicleRecord, 128)
        probeButton("Spawn vehicle in front of V", "vehicleSpawn")
        probeButton("Seat probe as driver", "seatDriver")
        ImGui.SameLine()
        probeButton("... as passenger", "seatPassenger")
        probeButton("Drive probe vehicle 15 m (teleport every frame)", "vehicleDrive")
    end
end

registerForEvent("onInit", function()
    log("coop-dev loaded")
end)

registerForEvent("onOverlayOpen", function() panel.overlayOpen = true end)
registerForEvent("onOverlayClose", function() panel.overlayOpen = false end)

registerForEvent("onShutdown", function()
    if panel.mirror then
        pcall(stopMirror)
    end
end)

registerForEvent("onUpdate", function()
    if panel.mirror then
        local ok, err = pcall(updateMirror)
        if not ok then
            log("mirror update failed: " .. tostring(err))
            pcall(stopMirror)
        end
    end
    updatePlacementTest()
    updateVehicleDrive()

    local now = os.clock()
    for i = #panel.checks, 1, -1 do
        local check = panel.checks[i]
        if now >= check.at then
            table.remove(panel.checks, i)
            try(check.label, check.fn)
        end
    end
end)

registerForEvent("onDraw", function()
    if not panel.overlayOpen then
        return
    end
    if panel.probeRecord == "" and Game.GetPlayer() then
        panel.probeRecord = lookalikeRecord()
    end
    if ImGui.Begin("Co-op (dev)") then
        if ImGui.BeginTabBar("coopTabs") then
            if ImGui.BeginTabItem("Session") then
                drawSession()
                ImGui.EndTabItem()
            end
            if ImGui.BeginTabItem("Direct drive") then
                drawDirectDrive()
                ImGui.EndTabItem()
            end
            if ImGui.BeginTabItem("Probes") then
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
