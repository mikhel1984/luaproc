/*
** auxiliary functions for lua processes
** 
*/

#ifndef _LUA_LUAPROC_AUX_H_
#define _LUA_LUAPROC_AUX_H_

#include <time.h>

typedef struct timespec timespec;

/* compare times */
int lpaux_time_cmp (timespec* t1, timespec* t2);

/* increase time */
void lpaux_time_inc (timespec* t, timespec* diff);

/* decrease time */
void lpaux_time_dec (timespec* t, timespec* diff);

/* split to seconds and nanoseconds */
timespec lpaux_time_period (double sec);

#endif 
