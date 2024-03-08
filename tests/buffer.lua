-- emulate 'buffered' channel 

luaproc = require "luaproc"

luaproc.setnumworkers( 1 )

local function buff_channel (inchan, len)
  -- prepare channels
  local ok, err = luaproc.newchannel(inchan)
  if not ok then return ok, err end
  local outchan = inchan..'_out'
  ok, err = luaproc.newchannel(outchan)
  if not ok then return ok, err end
  -- new process for buffer
  ok, err = luaproc.newproc(function ()
    local tbl = require 'table'
    local buf = {}
    while luaproc.isopen(inchan) or #buf > 0 do
      -- block when empty
      local v = tbl.pack(luaproc.receive(inchan, #buf > 0))
      if v[1] ~= nil then
        tbl.insert(buf, 1, v)  -- save data
      end
      if #buf < len then
        -- non-block
        if luaproc.broadcast(outchan, tbl.unpack(buf[#buf])) then
          tbl.remove(buf)
        end
        luaproc.sleep(0.001)  -- allow others to work        
      else
        -- block when full
        luaproc.send(outchan, tbl.unpack(tbl.remove(buf)))
      end
    end 
    luaproc.delchannel(outchan)
  end)
  if not ok then return ok, err end
  return outchan
end


local cin = 'c0'
local cout = assert(buff_channel(cin, 3))

assert(
luaproc.newproc(function ()
  while luaproc.isopen(cout) do
    print('proc2', luaproc.receive(cout))
    luaproc.sleep(1.0)    
  end  
end))

for i = 1, 5 do
  luaproc.send(cin, i, i*i)
  print('proc1 send', i, i*i)
end

luaproc.delchannel(cin)

