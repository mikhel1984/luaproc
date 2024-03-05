luaproc = require "luaproc"

local p = luaproc.period(1.0)

for i = 1, 5 do
  luaproc.sleep(p)
  print(i)
end

