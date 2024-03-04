luaproc = require "luaproc"

-- create an additional worker
luaproc.setnumworkers( 2 )

luaproc.newchannel('c1')

luaproc.newproc(function () 
  for i = 1, 5 do
    luaproc.send('c1', i)
    luaproc.sleep(1.0)
  end
end)

luaproc.newproc(function () 
  for i = 1, 5 do
    luaproc.send('c1', i)
    luaproc.sleep(0.9)
  end
end)

local j = 0
while j < 5 do
  j = luaproc.receive('c1')
  print(j)
end
