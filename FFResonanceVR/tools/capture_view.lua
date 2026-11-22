-- Cube-map colour faces from the game's own main view (UE4SS, flat game): a CameraActor becomes the view target with
-- a horizontal FOV of 2*atan(16/9) = 121.3 deg, so the centre square of each 16:9 HighResShot is a 90x90 deg face.
-- The main view has everything a SceneCapture lacks here (Lumen GI, eye adaptation, the game's post effects).
-- Exposure adapts on the forward face, then is frozen (adaptation speed 0) so the faces match.
-- Set VIEW = { x, y, z, yaw, settle = seconds per face, first = seconds before the first face } first.
-- Takes the shots in the order f r b l u d and prints "captured".
local v = VIEW
local pc = FindFirstOf("PlayerController")
local world = pc:GetWorld()
local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics")
local ksl = StaticFindObject("/Script/Engine.Default__KismetSystemLibrary")
local cls = StaticFindObject("/Script/Engine.CameraActor")
-- view-dependent effects would leave seams between the faces (this throwaway test session keeps them off)
for _, c in ipairs({ "ShowFlag.LensFlares 0", "ShowFlag.LightShafts 0", "ShowFlag.Vignette 0", "ShowFlag.Grain 0",
                     "r.MotionBlurQuality 0", "r.DepthOfFieldQuality 0", "r.SceneColorFringeQuality 0" }) do
    ksl:ExecuteConsoleCommand(pc, c, nil)
end
-- light-beam cards (Niagara "GodRay" effects): in a surround view they read as long streaks across the sky
for _, c in ipairs(FindAllOf("NiagaraComponent") or {}) do
    pcall(function()
        local n = c.Asset and c.Asset:IsValid() and c.Asset:GetFName():ToString() or ""
        for _, pat in ipairs(v.hide or { "GodRay", "Water", "Fountain" }) do
            if n:find(pat) then c:SetVisibility(false, true); c:SetHiddenInGame(true, true) end
        end
    end)
end
-- people, birds and their speech bubbles (they stay hidden for the depth capture that follows); fountain water (thin
-- streams the depth capture does not see: they would sit on whatever is behind them and flicker in the headset)
for _, a in ipairs(FindAllOf("Actor") or {}) do
    pcall(function()
        local n = a:GetClass():GetFName():ToString()
        for _, pat in ipairs(v.hide_actors or { "MapUnit_NPC", "MapUnit_Player", "Gimmick_Bird", "ChatTrigger", "WaterFountain" }) do
            if n:find(pat) then a:SetActorHiddenInGame(true) end
        end
    end)
end
-- the units' sprites (SpriteStudio player component) and blob shadows are components that ignore the actor's hidden flag
if v.hide_units ~= false then
    for _, a in ipairs(FindAllOf("CPP_Unit") or {}) do
        for _, p in ipairs({ "m_pSsPlayerComponent", "m_pDecalComponent", "m_pShadowComponent", "m_pBillboardComponent" }) do
            pcall(function() local c = a[p]; if c and c:IsValid() then c:SetVisibility(false, true); c:SetHiddenInGame(true, true) end end)
        end
    end
end
-- every sprite (people, animals, pixel props): the map's visibility controller turns hidden ones back on, so they are
-- also shrunk to nothing, and this runs again before every shot (destroying them crashes the game)
local function hide_sprites()
    if v.hide_units == false then return end
    for _, c in ipairs(FindAllOf("CPP_MuchaSsPlayerComponent") or {}) do
        pcall(function() c:SetVisibility(false, true); c:SetHiddenInGame(true, true); c:SetWorldScale3D({ X = 0.001, Y = 0.001, Z = 0.001 }) end)
    end
    for _, a in ipairs(FindAllOf("CPP_Unit") or {}) do   -- their blob shadows
        for _, p in ipairs({ "m_pDecalComponent", "m_pShadowComponent" }) do
            pcall(function() local c = a[p]; if c and c:IsValid() then c:SetVisibility(false, true); c:SetHiddenInGame(true, true) end end)
        end
    end
end
hide_sprites()
for _, c in ipairs(FindAllOf("WidgetComponent") or {}) do pcall(function() c:SetVisibility(false, true); c:SetHiddenInGame(true, true) end) end
local xf = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = v.x, Y = v.y, Z = v.z }, Scale3D = { X = 1, Y = 1, Z = 1 } }
local cam = gs:BeginDeferredActorSpawnFromClass(world, cls, xf, 1, nil, 0)
gs:FinishSpawningActor(cam, xf, 0)
local cc = cam.CameraComponent
cc.FieldOfView = v.fov or 121.2876   -- wider with overscan (capture_view.ps1 crops the centre)
cc.bConstrainAspectRatio = false
-- fixed exposure for every face (VIEW.ev, from capture_view.ps1's calibration against the adapted forward view)
if v.ev then
    local pp = cc.PostProcessSettings
    pp.bOverride_AutoExposureMethod = true; pp.AutoExposureMethod = 2   -- AEM_Manual
    pp.bOverride_AutoExposureApplyPhysicalCameraExposure = true; pp.AutoExposureApplyPhysicalCameraExposure = false
    pp.bOverride_AutoExposureBias = true; pp.AutoExposureBias = v.ev
    cc.PostProcessBlendWeight = 1
end
local pawn = pc.Pawn
if pawn and pawn:IsValid() then pawn:SetActorHiddenInGame(true) end
local old = pc:GetViewTarget()
pc:SetViewTargetWithBlend(cam, 0, 0, 0, false)
local y = v.yaw or 0
local faces = { { "f", 0, y }, { "r", 0, y + 90 }, { "b", 0, y + 180 }, { "l", 0, y - 90 }, { "u", 90, y }, { "d", -90, y } }
if v.only_f then faces = { faces[1] } end
local function aim(f) cam:K2_SetActorLocationAndRotation({ X = v.x, Y = v.y, Z = v.z }, { Pitch = f[2], Yaw = f[3], Roll = 0 }, false, {}, true) end
aim(faces[1])
local step, t, i = 0.1, 0, 1
local wait = v.first or 4
LoopAsync(100, function()
    t = t + step
    if t < wait then return false end
    ExecuteInGameThread(function()
        local ok, e = pcall(function()
            hide_sprites()
            ksl:ExecuteConsoleCommand(pc, "HighResShot " .. (v.mult or 1), nil)
        end)
        if not ok then print("error: " .. tostring(e)) end
    end)
    i = i + 1
    t = 0; wait = v.settle or 2
    if i > #faces then
        ExecuteWithDelay(1500, function()
            ExecuteInGameThread(function()
                pc:SetViewTargetWithBlend(old, 0, 0, 0, false)
                if pawn and pawn:IsValid() then pawn:SetActorHiddenInGame(false) end
                cam:K2_DestroyActor()
                print("captured")
            end)
        end)
        return true
    end
    -- turn to the next face only after the shot has been taken (next frame)
    ExecuteWithDelay(300, function() ExecuteInGameThread(function() aim(faces[i]) end) end)
    return false
end)
