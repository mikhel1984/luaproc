-- make timer, synchronise processes

luaproc = require "luaproc"

luaproc.setnumworkers( 1 )

local function new_timer (ch, sec)
  -- prepare channel
  local ok, err = luaproc.newchannel(ch)
  if not ok then return ok, err end
  -- run timer
  return luaproc.newproc(function ()
    local p = luaproc.period(sec)
    while luaproc.isopen(ch) do
      luaproc.sleep(p)
      luaproc.broadcast(ch, true)
    end
  end)
end

local t1 = 't1'
assert(new_timer(t1, 1.0))

-- run another process
assert(
luaproc.newproc(function ()
  while luaproc.isopen(t1) do
    luaproc.receive(t1)
    print('second proc')
  end  
end))

-- main 
for i = 1, 5 do
  luaproc.receive(t1)
  print('main proc')
end
luaproc.delchannel(t1)
