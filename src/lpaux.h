/*
** auxiliary functions for lua processes
** 
*/

#ifndef _LUA_LUAPROC_AUX_H_
#define _LUA_LUAPROC_AUX_H_

#include <time.h>

int lpaux_time_cmp (timespec* t1, timespec* t2);

void lpaux_time_inc (timespec* t, timespec* diff);

#endif 
