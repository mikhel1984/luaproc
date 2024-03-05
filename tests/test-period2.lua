luaproc = require "luaproc"

-- create an additional worker
luaproc.setnumworkers( 1 )

luaproc.newchannel('c1')

luaproc.newproc(function () 
  for i = 1, 4 do
    luaproc.send('c1', i, 'proc1')
    luaproc.sleep(1.0)
  end
end)

luaproc.newproc(function () 
  local p = luaproc.period(1.0)
  for i = 1, 5 do
    luaproc.send('c1', i, 'proc2')
    luaproc.sleep(p)
  end
end)

local j = 0
while j < 5 do
  j, src = luaproc.receive('c1')
  print(j, 'from', src)
end
