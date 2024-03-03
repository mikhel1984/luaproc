luaproc = require "luaproc"

-- create an additional worker
luaproc.setnumworkers( 2 )

ch = luaproc.newchannel('c1')

-- process 1
luaproc.newproc( function ()
  luaproc.send('c1', 'Hello!')
  print(luaproc.receive('c1'))
end)

local fn = function (a, b, c)
  print(luaproc.receive('c1'))
  luaproc.send('c1', a, b, c)
end

-- process 2
luaproc.newproc(fn, 1, 'two', 3)
