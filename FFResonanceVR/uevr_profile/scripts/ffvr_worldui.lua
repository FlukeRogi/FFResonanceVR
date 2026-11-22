-- FFResonanceVR: battle unit markers (HP gauges, letters, target cursors) at the depth of their unit.
--
-- The game draws each battle unit's UI (WBP_Btl_UnitUILayout, in the unit's UnitUIWidget component) in Screen space:
-- flat over the image, which in the VR window puts every marker at screen depth instead of at its enemy. A component
-- that registered in Screen space cannot draw in the world (its world renderer is only created at registration), so
-- for each battle unit this adds a new WidgetComponent that registers in World space and moves the game's own widget
-- object into it (the game keeps updating the same widget). Every tick the new component is turned parallel to the
-- view and placed on the line of sight to its unit (see DEPTH_FRACTION), scaled to keep its on-screen size.
-- Setting: second line of data\ffvr_camera.txt = marker depth fraction, 0 = off (VR menu "3D Battle Markers": Off /
-- Standard 0.4 / Close 0.9); third line = the
-- window's horizontal tan(half FOV), used for the size.
local api = uevr.api
local Hook = uevr.types.UObjectHook

local enabled = true        -- derived from depth_fraction > 0
local tan_half = 0.64       -- window half-width / distance (default screen 3.2 m at 2.5 m)
local DPI = 1.11            -- size match with the game's Screen-space markers (measured against the flat HUD)
local UI_W = 2560           -- UI width in pixels that the window shows across its width
-- Markers are depth-tested in the scene, so grass and rocks between camera and unit would hide them (the flat HUD drew
-- them on top). They are placed on the line of sight to their unit at this fraction of its distance and scaled for
-- that distance: the same position and size on screen, in front of most occluders, and - beyond ~10 m stereo depth
-- hardly changes - still seen at practically the unit's depth.
local depth_fraction = 0.4   -- from the setting: Standard 0.4, Close 0.9 (0 = off)
-- The game's layout hangs parts of the marker outside its 500x500 canvas (damage numbers and the BREAK logo centred on
-- the anchor, the selection pointer to the right); a World-space widget is clipped to its canvas, so the 3D marker
-- gets a bigger canvas with the layout lifted inside it and the pivot moved to match (same place on the unit).
-- (wide: the target name plate starts right of the anchor and grows with the name; at 1200 the letter after a long
-- name was cut off)
local CANVAS_W, CANVAS_H, LIFT = 2400, 1000, 300

local wc_class = nil
local moved = {}            -- [original component] = { nc = new component, w = widget }
local scan_t, setting_t = 0, 0

local function alive(o) return o ~= nil and Hook.exists(o) end
local v2_struct = nil
local function vec2(x, y)
    v2_struct = v2_struct or api:find_uobject("ScriptStruct /Script/CoreUObject.Vector2D")
    local s = StructObject.new(v2_struct); s.X = x; s.Y = y
    return s
end

local function read_setting()
    local f = io.open("ffvr_camera.txt", "r")
    if not f then return end
    f:read("*l")
    local on = tonumber(f:read("*l") or "")
    local t = tonumber(f:read("*l") or "")
    f:close()
    if on then
        if on == 1 then on = 0.4 end   -- older setting files: 1 = on
        depth_fraction = on
        enabled = on > 0
    end
    if t and t > 0 then tan_half = t end
end

-- Markers are drawn on top of the scene, like the flat HUD (still at their stereo depth): otherwise a nearer enemy or
-- the unit's own sprite cuts letters and gauges off. The game's own world-space widgets (field item heads) use the
-- masked material, where this flag has no effect (the engine only honours it for translucent materials), so only the
-- translucent markers change.
local widget_base = nil
local function set_on_top(on)
    widget_base = widget_base or api:find_uobject("Material /Engine/EngineMaterials/Widget3DPassThrough.Widget3DPassThrough")
    if widget_base then widget_base.bDisableDepthTest = on end
end

local function move_into_world(o)
    set_on_top(true)
    local owner = o:GetOwner()
    local w = o:GetUserWidgetObject()
    if not owner or not w then return end
    local nc = api:add_component_by_class(owner, wc_class, true)
    if not nc then return end
    nc.Space = 0              -- World
    nc.bIsTwoSided = true
    nc.BlendMode = 2          -- Transparent
    -- (struct arguments need real struct objects in UEVR's Lua, so these are set as properties before registering)
    nc.DrawSize.X = CANVAS_W; nc.DrawSize.Y = CANVAS_H
    nc.Pivot.X = 0.5; nc.Pivot.Y = (CANVAS_H - LIFT) / CANVAS_H
    owner:FinishAddComponent(nc, false, owner:GetTransform())
    -- lift the whole marker inside the bigger canvas (render translation of the root: the game animates the layout's
    -- own position every frame, but not the root's transform)
    local root = w.WidgetTree.RootWidget
    if root then root:SetRenderTranslation(vec2(0, -LIFT)) end
    o:SetWidget(nil)
    nc:SetWidget(w)
    moved[o] = { nc = nc, w = w, root = root }
end

local function restore_all()
    for o, m in pairs(moved) do
        pcall(function()
            if alive(m.nc) then m.nc:SetWidget(nil); m.nc:K2_DestroyComponent(m.nc) end
            if m.root and alive(m.root) then m.root:SetRenderTranslation(vec2(0, 0)) end
            if alive(o) and alive(m.w) then o:SetWidget(m.w) end
        end)
    end
    moved = {}
    pcall(set_on_top, false)
end

local function scan()
    wc_class = wc_class or api:find_uobject("Class /Script/UMG.WidgetComponent")
    if not wc_class then return end
    -- forget units that are gone
    for o, m in pairs(moved) do
        if not alive(o) or not alive(m.nc) then moved[o] = nil end
    end
    for _, o in ipairs(wc_class:get_objects_matching(false)) do
        if moved[o] == nil and o.Space == 1 and o:get_fname():to_string() == "UnitUIWidget" and not o:get_full_name():find("Default__") then
            pcall(move_into_world, o)
        end
    end
end

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)
    setting_t = setting_t - delta
    if setting_t <= 0 then
        setting_t = 1
        local was = enabled
        pcall(read_setting)
        if was and not enabled then restore_all() end
    end
    if not enabled then return end
    scan_t = scan_t - delta
    if scan_t <= 0 then scan_t = 0.25; pcall(scan) end
    if next(moved) == nil then return end
    pcall(function()
        local cm = api:get_player_controller(0).PlayerCameraManager
        local c0 = cm:GetCameraLocation()
        -- the view position: the game's camera plus the battle-zoom camera move (ffvr_camera.lua)
        local z = _G.FFVR_ZOOM
        local c = { X = c0.X + (z and z.ox or 0), Y = c0.Y + (z and z.oy or 0), Z = c0.Z + (z and z.oz or 0) }
        local r = cm:GetCameraRotation()
        local cp, cy = math.rad(r.Pitch), math.rad(r.Yaw)
        local fx, fy, fz = math.cos(cp) * math.cos(cy), math.cos(cp) * math.sin(cy), math.sin(cp)
        local k = 2 * tan_half / UI_W * DPI   -- world size of one UI pixel per cm of depth
        local rot = Vector3d.new(-r.Pitch, r.Yaw + 180, 0)
        for o, m in pairs(moved) do
            if alive(o) and alive(m.nc) then
                m.nc:SetVisibility(o:IsVisible(), false)
                -- the game's own anchor for the marker: where its Screen-space component sits (it projected that point)
                local b = o:K2_GetComponentLocation()
                local depth = (b.X - c.X) * fx + (b.Y - c.Y) * fy + (b.Z - c.Z) * fz
                if depth > 50 then
                    local f = depth_fraction
                    local s = depth * f * k
                    m.nc:K2_SetWorldRotation(rot, false, {}, true)
                    m.nc:SetWorldScale3D(Vector3d.new(s, s, s))
                    m.nc:K2_SetWorldLocation(Vector3d.new(c.X + (b.X - c.X) * f, c.Y + (b.Y - c.Y) * f, c.Z + (b.Z - c.Z) * f), false, {}, true)
                end
            end
        end
    end)
end)
