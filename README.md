# luaproc - A concurrent programming library for Lua

## Original version

[https://github.com/askyrme/luaproc](https://github.com/askyrme/luaproc)

## Updates

* Lua 5.3+
* c11 _threads_ instead of _pthread_ library
* Allow function with arguments

## Compatibility

This version is compatible with Lua 5.3 and 5.4.

## API

**`luaproc.newproc( string lua_code )`**

**`luaproc.newproc( function f, [arg1], [arg2], [...] )`**

Creates a new Lua process to run the specified string of Lua code or the
specified Lua function. Returns true if successful or nil and an error message
if failed. The only libraries loaded in new Lua processes are luaproc itself and
the standard Lua base and package libraries. The remaining standard Lua
libraries (io, os, table, string, math, debug, coroutine and utf8) are
pre-registered and can be loaded with a call to the standard Lua function
`require`. 

When additional arguments are defined, the process executes function 
_f(arg1, arg2,...)_. The types of arguments are the same as in 'send/receive'
functions.

**`luaproc.setnumworkers( int number_of_workers )`**

Sets the number of active workers (pthreads) to n (default = 1, minimum = 1).
Creates and destroys workers as needed, depending on the current number of
active workers. No return, raises error if worker could not be created. 

**`luaproc.getnumworkers( )`**

Returns the number of active workers (pthreads). 

**`luaproc.wait( )`**

Waits until all Lua processes have finished, then continues program execution.
It only makes sense to call this function from the main Lua script. Moreover,
this function is implicitly called when the main Lua script finishes executing.
No return. 

**`luaproc.recycle( int maxrecycle )`**

Sets the maximum number of Lua processes to recycle. Returns true if successful
or nil and an error message if failed. The default number is zero, i.e., no Lua
processes are recycled. 

**`luaproc.send( string channel_name, msg1, [msg2], [msg3], [...] )`**

Sends a message (tuple of boolean, nil, number or string values) to a channel.
Returns true if successful or nil and an error message if failed. Suspends
execution of the calling Lua process if there is no matching receive. 

**`luaproc.receive( string channel_name, [boolean asynchronous] )`**

Receives a message (tuple of boolean, nil, number or string values) from a
channel. Returns received values if successful or nil and an error message if
failed. Suspends execution of the calling Lua process if there is no matching
receive and the async (boolean) flag is not set. The async flag, by default, is
not set. 

**`luaproc.newchannel( string channel_name )`**

Creates a new channel identified by string name. Returns true if successful or
nil and an error message if failed.

**`luaproc.delchannel( string channel_name )`**

Destroys a channel identified by string name. Returns true if successful or nil
and an error message if failed. Lua processes waiting to send or receive
messages on destroyed channels have their execution resumed and receive an error
message indicating the channel was destroyed. 

**`luaproc.sleep( double seconds )`**

**`luaproc.sleep( userdata period )`**

Stop execution for some time. The period can be defined using positive number or
userdata object. In the second case time of awakening is estimated from moment of
the object creation, which makes it more precise for repeated calls.

**'luaproc.period( double seconds )'**

Creates an object with constant period.

## License

Copyright © 2008-2015 Alexandre Skyrme, Noemi Rodriguez, Roberto Ierusalimschy.
All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
the Software, and to permit persons to whom the Software is furnished to do so,
subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. 
