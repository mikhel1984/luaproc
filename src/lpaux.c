/*
** auxiliary functions for lua processes
** 
*/

#include "lpaux.h"

#define NSINSEC 1000000000

/* compare times */
int lpaux_time_cmp (timespec* t1, timespec* t2)
{
  return (t1->tv_sec < t2->tv_sec) ? -1
       : (t1->tv_sec > t2->tv_sec) ?  1
       : (t1->tv_nsec < t2->tv_nsec) ? -1 
       : (t1->tv_nsec > t2->tv_nsec) ?  1 : 0;
}

/* increase time */
void lpaux_time_inc (timespec* t, timespec* diff)
{
  t->tv_sec += diff->tv_sec;
  t->tv_nsec += diff->tv_nsec;
  if ( t->tv_nsec >= NSINSEC ) {
    t->tv_nsec -= NSINSEC;
    t->tv_sec += 1;
  }
}

/* split to seconds and nanoseconds */
timespec lpaux_time_period (double sec)
{
  timespec t;
  t.tv_sec = (int) sec;
  t.tv_nsec = (int) ((sec - t.tv_sec) * 1E9);
  return t;
}
