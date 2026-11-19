#!/usr/local/bin/lua
-- lua-hello: greet the running system (DER/32 test package)
local p = io.popen("uname -s")
local sys = p:read("l")
p:close()
print(("Hello from %s on %s"):format(_VERSION, sys))
