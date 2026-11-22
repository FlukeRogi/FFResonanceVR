-- FFResonanceVR: runs commands written to <UEVR profile>\data\ffvr_console.txt (one per line), then empties the file.
--   <console command>      executed with the engine console (e.g. r.DepthOfFieldQuality 0)
--   lua <code>             Lua code (development); print() output and errors go to data\ffvr_out.txt
-- (UEVR's Lua io only reaches <profile>\data, by relative path.)
local api = uevr.api
local path = "ffvr_console.txt"
local acc = 0

local function out(...)
    local t = {}
    for i = 1, select("#", ...) do t[#t + 1] = tostring((select(i, ...))) end
    local f = io.open("ffvr_out.txt", "a")
    if f then f:write(table.concat(t, "  ") .. "\n"); f:close() end
end
FFVR_OUT = out

local function poll()
    local f = io.open(path, "r")
    if not f then return end
    local text = f:read("*a")
    f:close()
    if text == nil or text == "" then return end
    local w = io.open(path, "w")
    if w then w:close() end
    for line in text:gmatch("[^\r\n]+") do
        if line:match("^lua ") then
            local fn, err = load(line:sub(5), "ffvr", "t", setmetatable({ print = out }, { __index = _G }))
            if not fn then out("load error: " .. tostring(err))
            else
                local ok, e = pcall(fn)
                if not ok then out("error: " .. tostring(e)) end
            end
        elseif not line:match("^%s*[#;]") then
            api:execute_command(line)
        end
    end
end

uevr.sdk.callbacks.on_pre_engine_tick(function(engine, delta)
    acc = acc + delta
    if acc < 0.25 then return end
    acc = 0
    local ok, err = pcall(poll)
    if not ok then out("[ffvr_console] " .. tostring(err)) end
end)
