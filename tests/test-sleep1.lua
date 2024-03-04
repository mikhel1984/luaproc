luaproc = require "luaproc"

for i = 1, 5 do
  luaproc.sleep(1.0)
  print(i)
end

