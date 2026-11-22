-- Captures a cube map (6 faces) of the current map with the game's own renderer (UE4SS, flat game).
-- Set CAP = { dir = [[out folder]], x =, y =, z =, size =, yaw =, depth = false } before running
-- (tools\capture_scene.ps1 does this). Faces: f (yaw), r (yaw+90), b (yaw+180), l (yaw-90), u (pitch +90),
-- d (pitch -90); 90 deg FOV each. Colour: RGBA8 + SCS_FinalColorLDR -> <face>.png. Depth (CAP.depth): RGBA16f +
-- SCS_SceneDepth (cm) -> <face>_depth (EXR).
-- Exposure: a scene capture has no eye-adaptation history, so dark scenes come out black. The capture keeps its
-- rendering state and looks forward for ~2.5 s so its auto exposure settles, then the exposure is frozen (adaptation
-- speed 0) for all six faces, so they match. Prints "captured" when done (asynchronous).
local cap = CAP
local pc = FindFirstOf("PlayerController")
local world = pc:GetWorld()
local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics")
local krl = StaticFindObject("/Script/Engine.Default__KismetRenderingLibrary")
local cls = StaticFindObject("/Script/Engine.SceneCapture2D")
local xf = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = cap.x, Y = cap.y, Z = cap.z }, Scale3D = { X = 1, Y = 1, Z = 1 } }
local a = gs:BeginDeferredActorSpawnFromClass(world, cls, xf, 1, nil, 0)
gs:FinishSpawningActor(a, xf, 0)
local sc = a.CaptureComponent2D
local rt = krl:CreateRenderTarget2D(world, cap.size, cap.size, cap.depth and 6 or 2, { R = 0, G = 0, B = 0, A = 1 }, false, false)
sc.FOVAngle = 90
sc.CaptureSource = cap.depth and 4 or 2
sc.bCaptureEveryFrame = false
sc.bCaptureOnMovement = false
sc.bAlwaysPersistRenderingState = true
sc.TextureTarget = rt
local pawn = pc.Pawn
if pawn and pawn:IsValid() then pawn:SetActorHiddenInGame(true) end
local y = cap.yaw or 0
local function aim(pitch, yaw)
    sc:K2_SetWorldLocationAndRotation({ X = cap.x, Y = cap.y, Z = cap.z }, { Pitch = pitch, Yaw = yaw, Roll = 0 }, false, {}, true)
end
local faces = { { "f", 0, y }, { "r", 0, y + 90 }, { "b", 0, y + 180 }, { "l", 0, y - 90 }, { "u", 90, y }, { "d", -90, y } }
local function finish()
    if not cap.depth then
        local pp = sc.PostProcessSettings
        pp.bOverride_AutoExposureSpeedUp = true; pp.AutoExposureSpeedUp = 0
        pp.bOverride_AutoExposureSpeedDown = true; pp.AutoExposureSpeedDown = 0
    end
    for _, f in ipairs(faces) do
        aim(f[2], f[3])
        sc:CaptureScene()
        krl:ExportRenderTarget(world, rt, cap.dir, cap.depth and (f[1] .. "_depth") or (f[1] .. ".png"))
    end
    if pawn and pawn:IsValid() then pawn:SetActorHiddenInGame(false) end
    a:K2_DestroyActor()
    print("captured")
end
if cap.depth or cap.warmup == 0 then
    finish()
else
    aim(cap.warm_pitch or 0, y)
    local n, total = 0, cap.warmup or 50
    LoopAsync(50, function()
        n = n + 1
        if n < total then
            ExecuteInGameThread(function() pcall(function() sc:CaptureScene() end) end)
            return false
        end
        ExecuteInGameThread(function()
            local ok, e = pcall(finish)
            if not ok then print("error: " .. tostring(e)) end
        end)
        return true
    end)
end
