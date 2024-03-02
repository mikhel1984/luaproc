luaproc = require "luaproc"

-- create an additional worker
luaproc.setnumworkers( 2 )

ch = luaproc.newchannel('c1')

-- process 1
luaproc.newproc(function ()
  luaproc.send('c1', 'Hello!')
  print(luaproc.receive('c1'))
end)

-- process 2
luaproc.newproc(function ()
  print(luaproc.receive('c1'))
  luaproc.send('c1', 'Hi!')
end)
