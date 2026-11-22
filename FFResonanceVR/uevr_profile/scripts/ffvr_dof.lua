-- FFResonanceVR: the game's depth of field in both eyes, with a strength setting.
--
-- 1. Both eyes. Under UEVR's native stereo the game's depth of field only reaches the first view while the game's own
--    anti-aliasing (FXAA, r.AntiAliasingMethod 1) is active; with a temporal method it works in both views. TSR (4)
--    costs a third of the frame rate at the window's resolution, TAA (2) costs nothing measurable and keeps the pixel
--    sprites crisp (the 3D scenery gets slightly softer). So while depth of field is on, TAA is kept active; when it
--    is off, the game's own method is put back. The game's value is whatever is seen that this script did not set.
--    FXAA together with depth of field is more than a missing blur: the second view then gets NaN blocks, and bloom
--    floods it. So this script alone switches r.DepthOfFieldQuality, in an order that never passes through that
--    combination (on: TAA first, depth of field a step later; off: depth of field first, FXAA a step later), and
--    user_script.txt starts the game with depth of field off until this script is running.
-- 2. Strength. The game's cameras are cine cameras; their aperture (f-stop) sets the blur size, which is inversely
--    proportional to it. For strength s (0..1] every cine camera's aperture is set to <the game's value> / s. The
--    game's own value is tracked per camera: whenever the aperture is not what this script last wrote, the game has
--    changed it and that becomes the new base. s = 1 leaves the game's values alone.
-- Setting: fourth line of data\ffvr_camera.txt = strength 0..1 (VR menu "Depth of Field"; 0 = off).
local api = uevr.api
local Hook = uevr.types.UObjectHook

local strength = 1.0
local have_setting = false
local setting_t, list_t, aa_t = 0, 0, 0
local cams = {}             -- [component] = { orig = game's aperture, set = what we wrote }
local cine_class, ksl = nil, nil

-- ---- anti-aliasing method ------------------------------------------------------------------------------------
local DOF_AA = 2            -- TAA
local DOF_QUALITY = 2       -- r.DepthOfFieldQuality while on
local game_aa = nil         -- the game's own method
local aa_is_ours = false

local function tick_aa()
    if not have_setting or api:get_player_controller(0) == nil then return end
    ksl = ksl or api:find_uobject("KismetSystemLibrary /Script/Engine.Default__KismetSystemLibrary")
    if not ksl then return end
    local cur = math.floor(ksl:GetConsoleVariableIntValue("r.AntiAliasingMethod") + 0.5)
    local dofq = math.floor(ksl:GetConsoleVariableIntValue("r.DepthOfFieldQuality") + 0.5)
    if game_aa == nil and cur == DOF_AA then
        -- first look and TAA is already on: left over from this script before a script reload; the game uses FXAA
        game_aa = 1; aa_is_ours = true
    elseif not (aa_is_ours and cur == DOF_AA) then
        game_aa = cur; aa_is_ours = false   -- the game (re)set it
    end
    -- one step per call, so each change has taken effect before the next one
    if strength > 0 then
        if cur ~= DOF_AA then api:execute_command("r.AntiAliasingMethod " .. DOF_AA); aa_is_ours = true
        elseif dofq ~= DOF_QUALITY then api:execute_command("r.DepthOfFieldQuality " .. DOF_QUALITY) end
    else
        if dofq ~= 0 then api:execute_command("r.DepthOfFieldQuality 0")
        elseif aa_is_ours and game_aa ~= nil then
            api:execute_command("r.AntiAliasingMethod " .. game_aa)
            aa_is_ours = false
        end
    end
end
-- ---- strength ------------------------------------------------------------------------------------------------
local function read_setting()
    local f = io.open("ffvr_camera.txt", "r")
    if not f then return end
    f:read("*l"); f:read("*l"); f:read("*l")
    local s = tonumber(f:read("*l") or "")
    f:close()
    if s then strength = math.max(0, math.min(1, s)); have_setting = true end
end

local function refresh_list()
    cine_class = cine_class or api:find_uobject("Class /Script/CinematicCamera.CineCameraComponent")
    if not cine_class then return end
    for c, _ in pairs(cams) do
        if not Hook.exists(c) then cams[c] = nil end
    end
    for _, c in ipairs(cine_class:get_objects_matching(false)) do
        if cams[c] == nil and not c:get_full_name():find("Default__") then cams[c] = {} end
    end
end

local function apply()
    -- off (0) is handled by the console variable; the cameras then keep the game's own values
    local s = strength > 0 and strength or 1
    for c, st in pairs(cams) do
        if Hook.exists(c) then
            local cur = c.currentAperture
            if cur and cur > 0 then
                if st.set == nil or math.abs(cur - st.set) > 1e-4 then st.orig = cur end   -- the game's own value
                local want = st.orig / s
                if math.abs(cur - want) > 1e-4 then
                    c:SetCurrentAperture(want)
                    st.set = c.currentAperture       -- (read back: the lens limits clamp it)
                else
                    st.set = cur
                end
            end
        end
    end
end

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)
    aa_t = aa_t - delta
    if aa_t <= 0 then aa_t = 0.25; pcall(tick_aa) end
    setting_t = setting_t - delta
    if setting_t <= 0 then setting_t = 0.5; pcall(read_setting) end
    list_t = list_t - delta
    if list_t <= 0 then list_t = 1.0; pcall(refresh_list) end
    pcall(apply)
end)
