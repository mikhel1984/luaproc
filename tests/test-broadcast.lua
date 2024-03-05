luaproc = require "luaproc"

-- create an additional worker
luaproc.setnumworkers( 2 )

luaproc.newchannel('c1')

-- process 1
luaproc.newproc( function ()
  while true do
  print('proc1', luaproc.receive('c1'))
  end
end)

luaproc.newproc(function ()
  while true do
  print('proc2', luaproc.receive('c1'))
  end
end)

for i = 1, 5 do
  luaproc.sleep(0.5)
  luaproc.broadcast('c1', i)
end
