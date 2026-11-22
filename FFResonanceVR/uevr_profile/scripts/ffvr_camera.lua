-- FFResonanceVR: battle-intro camera lift.
-- When a battle starts, the game cuts to a camera close to the ground with a narrow (telephoto) field of view and
-- sweeps it up to the battle view in about a second. The VR window shows a much wider view than the game's, so the
-- start of that sweep shows the plane under the landscape. This lifts the camera at the cut and eases the lift back
-- to zero over the sweep, so the sweep ends exactly on the game's battle view.
-- Setting: data\ffvr_camera.txt holds the lift in metres (0 = off); written by the VR menu.
--
-- Battle zoom (optional, VR menu "Battle Zoom"; fifth line of ffvr_camera.txt, 1 = on).
-- The window always shows the same angle of the world, whatever field of view the game's camera has, so the game's
-- zooms (battle intro, attack and skill close-ups, the battle view itself is narrower than the field view) do nothing
-- in VR. With this option the camera is moved forward along its line of sight instead: for a game field of view f the
-- subject should look m = tan(field FOV / 2) / tan(f / 2) times bigger than in the field view, which it does when the
-- camera has covered the fraction 1 - 1/m of the distance to it. The subject distance is the depth of the battle
-- units, weighted towards the centre of the view. Depth and scale stay natural (it is a real camera move, not a
-- magnified picture). Only in battles (while battle units exist); smoothed, but it follows camera cuts at once.
local api = uevr.api

local LIFT_M = 6.0          -- default lift at the start of the sweep (metres)
local SWEEP_S = 1.0         -- the game's intro sweep takes about this long
local lift_m = LIFT_M
local active, t, last_fov, last_loc, stable = false, 0, nil, nil, 0
local cur_lift = 0          -- unreal units, applied to every view

-- battle zoom
local FIELD_FOV = 50.83     -- the game's field-of-view in the field (the reference: no camera move at this value)
local MIN_SUBJECT = 300     -- never closer to the subject than this (unreal units)
local MAX_FRACTION = 0.7    -- never cover more than this fraction of the distance
local zoom_on = false
local unit_class, units, units_t = nil, {}, 0
local dolly, dolly_target = 0, 0          -- unreal units along the view direction
local fwd = { 0, 0, 0 }
local Hook = uevr.types.UObjectHook
FFVR_ZOOM = { on = false, battle = false, fov = 0, subject = 0, dolly = 0 }   -- for diagnostics (tools\gamelua.ps1)

local function read_setting()
    local f = io.open("ffvr_camera.txt", "r")
    if not f then return end
    local v = tonumber(f:read("*l") or "")
    f:read("*l"); f:read("*l"); f:read("*l")
    local z = tonumber(f:read("*l") or "")
    f:close()
    if v then lift_m = v end
    zoom_on = z ~= nil and z > 0
end
local setting_timer = 0

local function smoothstep(x) x = math.max(0, math.min(1, x)); return x * x * (3 - 2 * x) end

local function refresh_units(delta)
    units_t = units_t - delta
    if units_t > 0 then return end
    units_t = 0.25
    units = {}
    if not zoom_on then return end
    unit_class = unit_class or api:find_uobject("Class /Script/mucha.CPP_BtlUnit")
    if not unit_class then return end
    for _, u in ipairs(unit_class:get_objects_matching(false)) do
        if not u:get_full_name():find("Default__") then units[#units + 1] = u end
    end
end

-- the camera move for the game's current zoom; cut = the camera jumped this tick
local function update_zoom(delta, fov, l, r, cut)
    local cp, cy = math.rad(r.Pitch), math.rad(r.Yaw)
    fwd = { math.cos(cp) * math.cos(cy), math.cos(cp) * math.sin(cy), math.sin(cp) }
    dolly_target = 0
    local subject = 0
    if zoom_on and #units > 0 and fov > 1 and fov < 170 then
        -- depth of the units, weighted towards the view centre (half the game's half-angle as the falloff)
        local sigma = math.max(3, fov * 0.25)
        local wsum, dsum = 0, 0
        for _, u in ipairs(units) do
            if Hook.exists(u) then
                local p = u:K2_GetActorLocation()
                local vx, vy, vz = p.X - l.X, p.Y - l.Y, p.Z - l.Z
                local depth = vx * fwd[1] + vy * fwd[2] + vz * fwd[3]
                local dist = math.sqrt(vx * vx + vy * vy + vz * vz)
                if depth > 50 and dist > 1 then
                    local ang = math.deg(math.acos(math.max(-1, math.min(1, depth / dist))))
                    local w = math.exp(-(ang / sigma) * (ang / sigma))
                    wsum = wsum + w; dsum = dsum + w * depth
                end
            end
        end
        if wsum > 1e-6 then
            subject = dsum / wsum
            local m = math.tan(math.rad(FIELD_FOV * 0.5)) / math.tan(math.rad(fov * 0.5))
            local d = subject * (1 - 1 / m)
            d = math.min(d, subject * MAX_FRACTION, math.max(0, subject - MIN_SUBJECT))
            dolly_target = math.max(d, -subject * 0.5)
        end
    end
    if cut then dolly = dolly_target
    else dolly = dolly + (dolly_target - dolly) * math.min(1, delta / 0.12) end
    if math.abs(dolly) < 0.5 and dolly_target == 0 then dolly = 0 end
    FFVR_ZOOM.on = zoom_on; FFVR_ZOOM.battle = #units > 0; FFVR_ZOOM.fov = fov; FFVR_ZOOM.subject = subject; FFVR_ZOOM.dolly = dolly
    -- where the view really is, relative to the game's camera (ffvr_worldui.lua places the battle markers from it)
    FFVR_ZOOM.ox = fwd[1] * dolly; FFVR_ZOOM.oy = fwd[2] * dolly; FFVR_ZOOM.oz = fwd[3] * dolly
end

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)
    setting_timer = setting_timer - delta
    if setting_timer <= 0 then setting_timer = 1; pcall(read_setting) end
    local ok = pcall(function()
        local pc = api:get_player_controller(0)
        local cm = pc and pc.PlayerCameraManager
        if not cm then return end
        local fov = cm:GetFOVAngle()
        local l = cm:GetCameraLocation()
        refresh_units(delta)
        local cut = false
        if last_loc then
            local dx, dy, dz = l.X - last_loc.X, l.Y - last_loc.Y, l.Z - last_loc.Z
            cut = math.sqrt(dx * dx + dy * dy + dz * dz) > 300 or (last_fov ~= nil and math.abs(fov - last_fov) > 8)
        end
        update_zoom(delta, fov, l, cm:GetCameraRotation(), cut)
        if last_loc and last_fov then
            local dx, dy, dz = l.X - last_loc.X, l.Y - last_loc.Y, l.Z - last_loc.Z
            local jump = math.sqrt(dx * dx + dy * dy + dz * dz)
            -- a cut (camera teleports) into a narrower view = the start of a battle intro sweep
            if jump > 2000 and fov < last_fov - 3 and lift_m > 0 then
                active, t, stable = true, 0, 0
            end
        end
        if active then
            t = t + delta
            if last_fov and math.abs(fov - last_fov) < 0.005 then stable = stable + 1 else stable = 0 end
            local k = 1 - smoothstep(t / SWEEP_S)
            -- the game holds the start view for a few ticks before sweeping: only accept "settled" late in the sweep
            if (stable >= 3 and t > SWEEP_S * 0.6) or t > 3 then k = 0; active = false end
            cur_lift = lift_m * 100 * k
        else
            cur_lift = 0
        end
        last_fov = fov
        last_loc = { X = l.X, Y = l.Y, Z = l.Z }
    end)
    if not ok then cur_lift = 0; dolly = 0 end
end)

uevr.sdk.callbacks.on_post_calculate_stereo_view_offset(function(device, view_index, world_to_meters, position, rotation, is_double)
    if cur_lift ~= 0 then position.z = position.z + cur_lift end
    if dolly ~= 0 then
        position.x = position.x + fwd[1] * dolly; position.y = position.y + fwd[2] * dolly; position.z = position.z + fwd[3] * dolly
    end
end)
