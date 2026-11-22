-- One capture (UE4SS, flat game): SHOT = { dir, name, x, y, z, pitch, yaw, fov, size, source (2 colour, 1 depth), fmt }
local s = SHOT
local pc = FindFirstOf("PlayerController")
local world = pc:GetWorld()
local gs = StaticFindObject("/Script/Engine.Default__GameplayStatics")
local krl = StaticFindObject("/Script/Engine.Default__KismetRenderingLibrary")
local cls = StaticFindObject("/Script/Engine.SceneCapture2D")
local xf = { Rotation = { X = 0, Y = 0, Z = 0, W = 1 }, Translation = { X = s.x, Y = s.y, Z = s.z }, Scale3D = { X = 1, Y = 1, Z = 1 } }
local a = gs:BeginDeferredActorSpawnFromClass(world, cls, xf, 1, nil, 0)
gs:FinishSpawningActor(a, xf, 0)
local sc = a.CaptureComponent2D
local rt = krl:CreateRenderTarget2D(world, s.size or 1024, s.size or 1024, s.fmt or 2, { R = 0, G = 0, B = 0, A = 1 }, false, false)
sc.FOVAngle = s.fov or 90
sc.CaptureSource = s.source or 2
sc.bCaptureEveryFrame = false
sc.bCaptureOnMovement = false
sc.TextureTarget = rt
if s.ortho then sc.ProjectionType = 1; sc.OrthoWidth = s.ortho end
local pawn = pc.Pawn
if pawn and pawn:IsValid() then pawn:SetActorHiddenInGame(true) end
sc:K2_SetWorldLocationAndRotation({ X = s.x, Y = s.y, Z = s.z }, { Pitch = s.pitch or 0, Yaw = s.yaw or 0, Roll = 0 }, false, {}, true)
sc:CaptureScene()
krl:ExportRenderTarget(world, rt, s.dir, s.name)
if pawn and pawn:IsValid() then pawn:SetActorHiddenInGame(false) end
a:K2_DestroyActor()
print("shot " .. s.name)
