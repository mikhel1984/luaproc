-- make buffer in sender, use 'portable' form 

luaproc = require "luaproc"

luaproc.setnumworkers( 1 )

local bufsend = [[
return function (ch, buf, len)
  local table = require 'table'
  -- send data
  return function (...)
    table.insert(buf, 1, {...})
    if #buf < len then
      if luaproc.broadcast(ch, table.unpack(buf[#buf])) then
        table.remove(buf)
      end
      return true
    else
      return luaproc.send(ch, table.unpack(table.remove(buf)))
    end
  end
end]]


local ch = 'c0'
luaproc.newchannel(ch)

assert(
luaproc.newproc(function ()
  -- buffered sender 
  local maker = load(bufsend)()
  local buf = {}
  local sender = maker(ch, buf, 3)
  for i = 1, 6 do
    sender(i, i*i)
    print('proc2 send', i, i*i)
  end
  luaproc.delchannel(ch)  
end))

while luaproc.isopen(ch) do
  print('proc1 recv', luaproc.receive(ch))
  luaproc.sleep(1.0)    
end  

