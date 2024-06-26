/*
** luaproc API
** See Copyright Notice in luaproc.h
*/

#include <threads.h>
#include <stdlib.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "luaproc.h"
#include "lpsched.h"
#include "lpaux.h"

#define FALSE 0
#define TRUE  !FALSE
#define LUAPROC_CHANNELS_TABLE "channeltb"
#define LUAPROC_RECYCLE_MAX 0
#define RATE_MARKER 0xdecada42


#define requiref( L, modname, f, glob ) \
  { luaL_requiref( L, modname, f, glob ); lua_pop( L, 1 ); }

#define copynumber( Lto, Lfrom, i ) {\
  if ( lua_isinteger( Lfrom, i )) {\
    lua_pushinteger( Lto, lua_tonumber( Lfrom, i ));\
  } else {\
    lua_pushnumber( Lto, lua_tonumber( Lfrom, i ));\
  }\
}

/********************
 * global variables *
 *******************/

/* channel list mutex */
static mtx_t mutex_channel_list;  // destroy!

/* recycle list mutex */
static mtx_t mutex_recycle_list;

/* recycled lua process list */
static list recycle_list;

/* maximum lua processes to recycle */
static int recyclemax = LUAPROC_RECYCLE_MAX;

/* lua_State used to store channel hash table */
static lua_State *chanls = NULL;

/* lua process used to wrap main state. allows main state to be queued in
   channels when sending and receiving messages */
static luaproc mainlp;

/* main state matched a send/recv operation conditional variable */
cnd_t cond_mainls_sendrecv;

/* main state communication mutex */
static mtx_t mutex_mainls;

/***********************
 * register prototypes *
 ***********************/

static void luaproc_openlualibs( lua_State *L );
static int luaproc_create_newproc( lua_State *L );
static int luaproc_wait( lua_State *L );
static int luaproc_send( lua_State *L );
static int luaproc_receive( lua_State *L );
static int luaproc_create_channel( lua_State *L );
static int luaproc_destroy_channel( lua_State *L );
static int luaproc_set_numworkers( lua_State *L );
static int luaproc_get_numworkers( lua_State *L );
static int luaproc_recycle_set( lua_State *L );
static int luaproc_sleep( lua_State* L );
static int luaproc_period( lua_State* L );
static int luaproc_broadcast (lua_State* L);
static int luaproc_isopen (lua_State* L);
LUALIB_API int luaopen_luaproc( lua_State *L );
static int luaproc_loadlib( lua_State *L );

/***********
 * structs *
 ***********/

/* lua process */
struct stluaproc
{
  lua_State *lstate;
  int status;
  int args;
  timespec wake_up;
  channel *chan;
  luaproc *next;
};

/* communication channel */
struct stchannel
{
  list send;
  list recv;
  mtx_t mutex;
  cnd_t can_be_used;
};

typedef struct 
{
  int marker;
  timespec time;
  timespec period;

} lprate;

/* luaproc function registration array */
static const struct luaL_Reg luaproc_funcs[] = {
  { "newproc", luaproc_create_newproc },
  { "wait", luaproc_wait },
  { "send", luaproc_send },
  { "receive", luaproc_receive },
  { "newchannel", luaproc_create_channel },
  { "delchannel", luaproc_destroy_channel },
  { "setnumworkers", luaproc_set_numworkers },
  { "getnumworkers", luaproc_get_numworkers },
  { "recycle", luaproc_recycle_set },
  { "sleep", luaproc_sleep },
  { "period", luaproc_period },
  { "broadcast", luaproc_broadcast },
  { "isopen", luaproc_isopen },
  { NULL, NULL }
};

/******************
 * list functions *
 ******************/

/* insert a lua process in a (fifo) list */
void list_insert (list *l, luaproc *lp)
{
  if ( l->head == NULL ) {
    l->head = lp;
  } else {
    l->tail->next = lp;
  }
  l->tail = lp;
  lp->next = NULL;
  l->nodes++;
}

/* remove and return the first lua process in a (fifo) list */
luaproc *list_remove (list *l)
{
  if ( l->head != NULL ) {
    luaproc *lp = l->head;
    l->head = lp->next;
    l->nodes--;
    return lp;
  } else {
    return NULL; /* if list is empty, return NULL */
  }
}

/* return a list's node count */
int list_count (list *l)
{
  return l->nodes;
}

/* initialize an empty list */
void list_init (list *l)
{
  l->head = NULL;
  l->tail = NULL;
  l->nodes = 0;
}

/* sort by time when insert */
void list_time_insert (list* l, luaproc* lp)
{
  lp->next = NULL;
  if ( l->head == NULL ) {
    /* empty list */
    l->head = l->tail = lp;
  } else if ( lpaux_time_cmp( &lp->wake_up, &l->head->wake_up ) < 0 ) {
    /* smallest time, set first */
    lp->next = l->head;
    l->head = lp;
  } else {
    luaproc* ptr = l->head;
    while ( ptr->next != NULL ) {
      if ( lpaux_time_cmp( &lp->wake_up, &ptr->next->wake_up ) < 0 ){
        /* set between current and next */
        lp->next = ptr->next;
        ptr->next = lp;
        break;
      }
      ptr = ptr->next;
    }
    /* have not been inserted, add into tail */
    if ( lp->next == NULL ) {
      l->tail->next = lp;
      l->tail = lp;
    }
  }
  l->nodes++;
}

/* get next wake up time or NULL */
timespec* list_time_next (list* l)
{
  return (l->head == NULL) ? NULL : &l->head->wake_up;
}

/* get ready to wake up processes */
luaproc* list_time_ready (list* l, timespec* current)
{
  if ( l->head != NULL && lpaux_time_cmp( &l->head->wake_up, current ) < 1 ) {
    return list_remove( l );
  }
  return NULL;
}

/*********************
 * channel functions *
 *********************/

/* create a new channel and insert it into channels table */
static channel *channel_create (const char *cname)
{
  /* get exclusive access to channels list */
  mtx_lock( &mutex_channel_list );

  /* create new channel and register its name */
  lua_getglobal( chanls, LUAPROC_CHANNELS_TABLE );
  channel* chan = (channel *)lua_newuserdata( chanls, sizeof( channel ));
  lua_setfield( chanls, -2, cname );
  lua_pop( chanls, 1 );  /* remove channel table from stack */

  /* initialize channel struct */
  list_init( &chan->send );
  list_init( &chan->recv );
  mtx_init( &chan->mutex, mtx_plain );
  cnd_init( &chan->can_be_used );

  /* release exclusive access to channels list */
  mtx_unlock( &mutex_channel_list );

  return chan;
}

/*
   return a channel (if not found, return null).
   caller function MUST lock 'mutex_channel_list' before calling this function.
 */
static channel *channel_unlocked_get (const char *chname)
{
  lua_getglobal( chanls, LUAPROC_CHANNELS_TABLE );
  lua_getfield( chanls, -1, chname );
  channel* chan = (channel *)lua_touserdata( chanls, -1 );
  lua_pop( chanls, 2 );  /* pop userdata and channel */

  return chan;
}

/*
   return a channel (if not found, return null) with its (mutex) lock set.
   caller function should unlock channel's (mutex) lock after calling this
   function.
 */
static channel *channel_locked_get (const char *chname)
{
  /* get exclusive access to channels list */
  mtx_lock( &mutex_channel_list );

  /*
     try to get channel and lock it; if lock fails, release external
     lock ('mutex_channel_list') to try again when signaled -- this avoids
     keeping the external lock busy for too long. during the release,
     the channel may be destroyed, so it must try to get it again.
  */
  channel *chan = NULL;
  while (( chan = channel_unlocked_get( chname )) != NULL
    && mtx_trylock( &chan->mutex ) != 0)
  {
    cnd_wait( &chan->can_be_used, &mutex_channel_list );
  }

  /* release exclusive access to channels list */
  mtx_unlock( &mutex_channel_list );

  return chan;
}

/********************************
 * exported auxiliary functions *
 ********************************/

/* unlock access to a channel and signal it can be used */
void luaproc_unlock_channel (channel *chan)
{
  /* get exclusive access to channels list */
  mtx_lock( &mutex_channel_list );
  /* release exclusive access to operate on a particular channel */
  mtx_unlock( &chan->mutex );
  /* signal that a particular channel can be used */
  cnd_signal( &chan->can_be_used );
  /* release exclusive access to channels list */
  mtx_unlock( &mutex_channel_list );
}

/* insert lua process in recycle list */
void luaproc_recycle_insert (luaproc *lp)
{
  /* get exclusive access to recycled lua processes list */
  mtx_lock( &mutex_recycle_list );

  /* is recycle list full? */
  if ( list_count( &recycle_list ) >= recyclemax ) {
    /* destroy state */
    lua_close( luaproc_get_state( lp ));
  } else {
    /* insert lua process in recycle list */
    list_insert( &recycle_list, lp );
  }

  /* release exclusive access to recycled lua processes list */
  mtx_unlock( &mutex_recycle_list );
}

/* queue a lua process that tried to send a message */
void luaproc_queue_sender (luaproc *lp)
{
  list_insert( &lp->chan->send, lp );
}

/* queue a lua process that tried to receive a message */
void luaproc_queue_receiver (luaproc *lp)
{
  list_insert( &lp->chan->recv, lp );
}

/********************************
 * internal auxiliary functions *
 ********************************/
static void luaproc_loadbuffer (
  lua_State *parent, luaproc *lp, const char *code, size_t len)
{
  /* load lua process' lua code */
  int ret = luaL_loadbuffer( lp->lstate, code, len, code );

  /* in case of errors, close lua_State and push error to parent */
  if ( ret != 0 ) {
    lua_pushstring( parent, lua_tostring( lp->lstate, -1 ));
    lua_close( lp->lstate );
    luaL_error( parent, lua_tostring( parent, -1 ));
  }
}

/* get elements betwee Lua states */
static int copy_data (lua_State* Lfrom, lua_State* Lto, int ind)
{
  const char* str = NULL;
  size_t len = 0;

  switch ( lua_type( Lfrom, ind )) {
    case LUA_TBOOLEAN:
      lua_pushboolean( Lto, lua_toboolean( Lfrom, ind ));
      break;
    case LUA_TNUMBER:
      copynumber( Lto, Lfrom, ind );
      break;
    case LUA_TSTRING: {
      str = lua_tolstring( Lfrom, ind, &len );
      lua_pushlstring( Lto, str, len );
      break;
    }
    case LUA_TNIL:
      lua_pushnil( Lto );
      break;
    default: /* value type not supported: table, function, userdata, etc. */
      return FALSE;
  }
  return TRUE;
}

/* copies values between lua states' stacks */
static int luaproc_copyvalues (lua_State *Lfrom, lua_State *Lto)
{
  int n = lua_gettop( Lfrom );

  /* ensure there is space in the receiver's stack */
  if ( lua_checkstack( Lto, n ) == 0 ) {
    lua_pushnil( Lto );
    lua_pushstring( Lto, "not enough space in the stack" );
    lua_pushnil( Lfrom );
    lua_pushstring( Lfrom, "not enough space in the receiver's stack" );
    return FALSE;
  }

  /* test each value's type and, if it's supported, copy value */
  for ( int i = 2; i <= n; i++ ) {
    if ( !copy_data( Lfrom, Lto, i )) {
      lua_settop( Lto, 1 );
      lua_pushnil( Lto );
      lua_pushfstring( Lto, "failed to receive value of unsupported type '%s'",
        luaL_typename( Lfrom, i ));
      lua_pushnil( Lfrom );
      lua_pushfstring( Lfrom, "failed to send value of unsupported type '%s'",
        luaL_typename( Lfrom, i ));
      return FALSE;
    }
  }
  return TRUE;
}

/* return the lua process associated with a given lua state */
static luaproc *luaproc_getself (lua_State *L)
{
  lua_getfield( L, LUA_REGISTRYINDEX, "LUAPROC_LP_UDATA" );
  luaproc* lp = (luaproc *)lua_touserdata( L, -1 );
  lua_pop( L, 1 );

  return lp;
}

/* create new lua process */
static luaproc *luaproc_new (lua_State *L)
{
  lua_State *lpst = luaL_newstate();  /* create new lua state */

  /* store the lua process in its own lua state */
  luaproc* lp = (luaproc *)lua_newuserdata( lpst, sizeof( struct stluaproc ));
  lua_setfield( lpst, LUA_REGISTRYINDEX, "LUAPROC_LP_UDATA" );
  luaproc_openlualibs( lpst );  /* load standard libraries and luaproc */
  /* register luaproc's own functions */
  requiref( lpst, "luaproc", luaproc_loadlib, TRUE );
  lp->lstate = lpst;  /* insert created lua state into lua process struct */

  return lp;
}

/* join schedule workers (called before exiting Lua) */
static int luaproc_join_workers (lua_State *L)
{
  sched_join_workers();

  /* destroy elements */
  mtx_destroy(&mutex_channel_list);
  mtx_destroy(&mutex_recycle_list);
  mtx_destroy(&mutex_mainls);
  cnd_destroy(&cond_mainls_sendrecv);

  lua_close( chanls );
  return 0;
}

/* writer function for lua_dump */
static int luaproc_buff_writer (
  lua_State *L, const void *buff, size_t size, void *ud)
{
  (void)L;
  luaL_addlstring( (luaL_Buffer *)ud, (const char *)buff, size );
  return 0;
}

/* copy arguments of the process function */
static int copy_arguments (lua_State* L, luaproc* p)
{
  int n = lua_gettop( L );
  if ( n > 1 ) {
    for ( int i = 2; i <= n; i++ ) {
      /* copy arguments */
      if ( !copy_data( L, p->lstate, i )) {
        lua_pushnil( L );
        lua_pushfstring( L, "failed to copy arg of unsupported type '%s'",
          luaL_typename( L, i ));
        return FALSE;
      }
    }
    p->args = n - 1;  /* update */
  }
  return TRUE;
}

/* copies upvalues between lua states' stacks */
static int luaproc_copyupvalues (
  lua_State *Lfrom, lua_State *Lto, int funcindex)
{
  /* test the type of each upvalue and, if it's supported, copy it */
  for ( int i = 1; lua_getupvalue( Lfrom, funcindex, i ) != NULL; i++ ) {
    if ( !copy_data( Lfrom, Lto, -1 )) {
      /* if upvalue is a table, check whether it is the global environment
         (_ENV) from the source state Lfrom. in case so, push in the stack of
         the destination state Lto its own global environment to be set as the
         corresponding upvalue; otherwise, treat it as a regular non-supported
         upvalue type. */
      if (lua_type( Lfrom, -1 ) == LUA_TTABLE ) {
        lua_pushglobaltable( Lfrom );
        int equal = lua_compare( Lfrom, -1, -2, LUA_OPEQ );
        lua_pop( Lfrom, 1 );
        if ( !equal ) {
          lua_pushnil( Lfrom );
          lua_pushfstring( Lfrom, "failed to copy upvalue of unsupported type '%s'",
            luaL_typename( Lfrom, -2 ));
          return FALSE;
        }
        lua_pushglobaltable( Lto );
      }
    }
    lua_pop( Lfrom, 1 );
    if ( lua_setupvalue( Lto, 1, i ) == NULL ) {
      lua_pushnil( Lfrom );
      lua_pushstring( Lfrom, "failed to set upvalue" );
      return FALSE;
    }
  }
  lua_pop( Lfrom, 1 );  /* remove function */
  return TRUE;
}


/*********************
 * library functions *
 *********************/

/* set maximum number of lua processes in the recycle list */
static int luaproc_recycle_set (lua_State *L)
{
  /* validate parameter is a non negative number */
  lua_Integer max = luaL_checkinteger( L, 1 );
  luaL_argcheck( L, max >= 0, 1, "recycle limit must be positive" );

  /* get exclusive access to recycled lua processes list */
  mtx_lock( &mutex_recycle_list );

  recyclemax = max;  /* set maximum number */

  /* remove extra nodes and destroy each lua processes */
  while ( list_count( &recycle_list ) > recyclemax ) {
    luaproc* lp = list_remove( &recycle_list );
    lua_close( lp->lstate );
  }
  /* release exclusive access to recycled lua processes list */
  mtx_unlock( &mutex_recycle_list );

  return 0;
}

/* wait until there are no more active lua processes */
static int luaproc_wait (lua_State *L)
{
  sched_wait();
  return 0;
}

/* set number of workers (creates or destroys accordingly) */
static int luaproc_set_numworkers (lua_State *L)
{
  /* validate parameter is a positive number */
  lua_Integer numworkers = luaL_checkinteger( L, -1 );
  luaL_argcheck( L, numworkers > 0, 1, "number of workers must be positive" );

  /* set number of threads; signal error on failure */
  if ( sched_set_numworkers( numworkers ) == LUAPROC_SCHED_PTHREAD_ERROR ) {
    luaL_error( L, "failed to create worker" );
  }

  return 0;
}

/* return the number of active workers */
static int luaproc_get_numworkers (lua_State *L)
{
  lua_pushnumber( L, sched_get_numworkers() );
  return 1;
}

/* make object for 'precise' sleeping */
static int luaproc_period (lua_State* L)
{
  double v = luaL_checknumber( L, 1 );
  luaL_argcheck( L, v > 0, 1, "period must be positive" );

  lprate* rate = (lprate*) lua_newuserdata( L, sizeof( lprate ));
  /* initialize */
  rate->marker = RATE_MARKER;
  rate->period = lpaux_time_period( v );
  timespec_get( &rate->time, TIME_UTC );

  return 1;
}

/* stop execution for some time */
static int luaproc_sleep (lua_State* L)
{
  timespec dur;
  int lt = lua_type( L, 1 );

  if ( lt == LUA_TNUMBER ) {
    double v = lua_tonumber( L, 1 );
    luaL_argcheck( L, v > 0, 1, "period must be positive" );
    dur = lpaux_time_period( v );

  } else if ( lt == LUA_TUSERDATA ) {
    lprate* rate = (lprate*) lua_touserdata( L, 1 );
    luaL_argcheck( L, rate->marker == RATE_MARKER, 1, "unexpected argument" );
    /* find period */
    timespec curr;
    timespec_get( &curr, TIME_UTC );
    while ( lpaux_time_cmp( &rate->time, &curr ) < 1 ) {
      lpaux_time_inc( &rate->time, &rate->period );
    }
    dur = rate->time;
    lpaux_time_dec( &dur, &curr );

  } else {
    luaL_error( L, "unexpected argument" );
  }

  if ( L == mainlp.lstate ) {
    thrd_sleep(&dur, NULL);
    return 0;
  } else {
    luaproc* self = luaproc_getself( L );
    if ( self != NULL ) {  // ???
      timespec_get( &self->wake_up, TIME_UTC );
      lpaux_time_inc( &self->wake_up, &dur );
      self->status = LUAPROC_STATUS_BLOCKED_SLEEP;
    }
    return lua_yield( L, 0 );
  }
}

/* create and schedule a new lua process */
static int luaproc_create_newproc (lua_State *L)
{
  luaproc *lp = NULL;

  /* check function argument type - must be function or string; in case it is
     a function, dump it into a binary string */
  int lt = lua_type( L, 1 );
  if ( lt == LUA_TFUNCTION ) {
    lua_rotate( L, 1, -1 );  /* function to the top */
    luaL_Buffer buff;
    luaL_buffinit( L, &buff );
    int d = lua_dump( L, luaproc_buff_writer, &buff, FALSE );
    if ( d != 0 ) {
      lua_pushnil( L );
      lua_pushfstring( L, "error %d dumping function to binary string", d );
      return 2;
    }
    luaL_pushresult( &buff );
    lua_insert( L, 1 );
  } else if ( lt != LUA_TSTRING ) {
    lua_pushnil( L );
    lua_pushfstring( L, "cannot use '%s' to create a new process",
      luaL_typename( L, 1 ));
    return 2;
  }

  /* get pointer to code string */
  size_t len;
  const char* code = lua_tolstring( L, 1, &len );

  /* get exclusive access to recycled lua processes list */
  mtx_lock( &mutex_recycle_list );

  /* check if a lua process can be recycled */
  if ( recyclemax > 0 ) {
    lp = list_remove( &recycle_list );
    /* otherwise create a new lua process */
    if ( lp == NULL ) {
      lp = luaproc_new( L );
    }
  } else {
    lp = luaproc_new( L );
  }

  /* release exclusive access to recycled lua processes list */
  mtx_unlock( &mutex_recycle_list );

  /* init lua process */
  lp->status = LUAPROC_STATUS_IDLE;
  lp->args   = 0;
  lp->chan   = NULL;

  /* load code in lua process */
  luaproc_loadbuffer( L, lp, code, len );

  /* if lua process is being created from a function, copy its upvalues and
     remove dumped binary string from stack */
  if ( lt == LUA_TFUNCTION ) {
    if ( luaproc_copyupvalues( L, lp->lstate, -1 ) == FALSE 
      || copy_arguments(L, lp) == FALSE ) 
    {
      luaproc_recycle_insert( lp );
      return 2;
    }
    lua_pop( L, 1 );
  }

  sched_inc_lpcount();   /* increase active lua process count */
  sched_queue_proc( lp );  /* schedule lua process for execution */
  lua_pushboolean( L, TRUE );

  return 1;
}

/* send a message to a lua process */
static int luaproc_send (lua_State *L)
{
  const char *chname = luaL_checkstring( L, 1 );
  channel* chan = channel_locked_get( chname );

  /* if channel is not found, return an error to lua */
  if ( chan == NULL ) {
    lua_pushnil( L );
    lua_pushfstring( L, "channel '%s' does not exist", chname );
    return 2;
  }

  /* remove first lua process, if any, from channel's receive list */
  luaproc* dstlp = list_remove( &chan->recv );

  if ( dstlp != NULL ) { /* found a receiver? */
    /* try to move values between lua states' stacks */
    int ret = luaproc_copyvalues( L, dstlp->lstate );
    /* -1 because channel name is on the stack */
    dstlp->args = lua_gettop( dstlp->lstate ) - 1;
    if ( dstlp->lstate == mainlp.lstate ) {
      /* if sending process is the parent (main) Lua state, unblock it */
      mtx_lock( &mutex_mainls );
      cnd_signal( &cond_mainls_sendrecv );
      mtx_unlock( &mutex_mainls );
    } else {
      /* schedule receiving lua process for execution */
      sched_queue_proc( dstlp );
    }
    /* unlock channel access */
    luaproc_unlock_channel( chan );
    if ( ret == TRUE ) { /* was send successful? */
      lua_pushboolean( L, TRUE );
      return 1;
    } else { /* nil and error msg already in stack */
      return 2;
    }

  } else {
    if ( L == mainlp.lstate ) {
      /* sending process is the parent (main) Lua state - block it */
      mainlp.chan = chan;
      luaproc_queue_sender( &mainlp );
      luaproc_unlock_channel( chan );
      mtx_lock( &mutex_mainls );
      cnd_wait( &cond_mainls_sendrecv, &mutex_mainls );
      mtx_unlock( &mutex_mainls );
      return mainlp.args;
    } else {
      /* sending process is a standard luaproc - set status, block and yield */
      luaproc* self = luaproc_getself( L );
      if ( self != NULL ) {
        self->status = LUAPROC_STATUS_BLOCKED_SEND;
        self->chan   = chan;
      }
      /* yield. channel will be unlocked by the scheduler */
      return lua_yield( L, lua_gettop( L ));
    }

  }
}

/* receive a message from a lua process */
static int luaproc_receive (lua_State *L)
{
  const char *chname = luaL_checkstring( L, 1 );

  /* get number of arguments passed to function */
  int nargs = lua_gettop( L );

  channel* chan = channel_locked_get( chname );
  /* if channel is not found, return an error to Lua */
  if ( chan == NULL ) {
    lua_pushnil( L );
    lua_pushfstring( L, "channel '%s' does not exist", chname );
    return 2;
  }

  /* remove first lua process, if any, from channels' send list */
  luaproc* srclp = list_remove( &chan->send );

  if ( srclp != NULL ) {  /* found a sender? */
    /* try to move values between lua states' stacks */
    int ret = luaproc_copyvalues( srclp->lstate, L );
    if ( ret == TRUE ) { /* was receive successful? */
      lua_pushboolean( srclp->lstate, TRUE );
      srclp->args = 1;
    } else {  /* nil and error_msg already in stack */
      srclp->args = 2;
    }
    if ( srclp->lstate == mainlp.lstate ) {
      /* if sending process is the parent (main) Lua state, unblock it */
      mtx_lock( &mutex_mainls );
      cnd_signal( &cond_mainls_sendrecv );
      mtx_unlock( &mutex_mainls );
    } else {
      /* otherwise, schedule process for execution */
      sched_queue_proc( srclp );
    }
    /* unlock channel access */
    luaproc_unlock_channel( chan );
    /* disconsider channel name, async flag and any other args passed
       to the receive function when returning its results */
    return lua_gettop( L ) - nargs;

  } else {  /* otherwise test if receive was synchronous or asynchronous */
    if ( lua_toboolean( L, 2 )) { /* asynchronous receive */
      /* unlock channel access */
      luaproc_unlock_channel( chan );
      /* return an error */
      lua_pushnil( L );
      lua_pushfstring( L, "no senders waiting on channel '%s'", chname );
      return 2;
    } else { /* synchronous receive */
      if ( L == mainlp.lstate ) {
        /*  receiving process is the parent (main) Lua state - block it */
        mainlp.chan = chan;
        luaproc_queue_receiver( &mainlp );
        luaproc_unlock_channel( chan );
        mtx_lock( &mutex_mainls );
        cnd_wait( &cond_mainls_sendrecv, &mutex_mainls );
        mtx_unlock( &mutex_mainls );
        return mainlp.args;
      } else {
        /* receiving process is a standard luaproc - set status, block and
           yield */
        luaproc* self = luaproc_getself( L );
        if ( self != NULL ) {
          self->status = LUAPROC_STATUS_BLOCKED_RECV;
          self->chan   = chan;
        }
        /* yield. channel will be unlocked by the scheduler */
        return lua_yield( L, lua_gettop( L ));
      }
    }

  }
}

static int luaproc_isopen (lua_State* L)
{
  const char *chname = luaL_checkstring( L, 1 );
  channel* chan = channel_locked_get( chname );
  if ( chan == NULL ) {
    lua_pushboolean( L, FALSE );
  } else {
    luaproc_unlock_channel( chan );
    lua_pushboolean( L, TRUE );
  }
  return 1;
}

static int luaproc_broadcast (lua_State* L)
{
  const char *chname = luaL_checkstring( L, 1 );
  channel* chan = channel_locked_get( chname );

  /* if channel is not found, return an error to lua */
  if ( chan == NULL ) {
    lua_pushnil( L );
    lua_pushfstring( L, "channel '%s' does not exist", chname );
    return 2;
  }
  
  int success = FALSE;
  while ( list_count( &chan->recv )) {
    luaproc* dst = list_remove( &chan->recv );
    int ret = luaproc_copyvalues( L, dst->lstate );
    dst->args = lua_gettop( dst->lstate ) - 1;
    if ( dst->lstate == mainlp.lstate ) {
      mtx_lock( &mutex_mainls );
      cnd_signal( &cond_mainls_sendrecv );
      mtx_unlock( &mutex_mainls );
    } else {
      sched_queue_proc( dst );
    }
    if ( ret == FALSE ) {
      luaproc_unlock_channel( chan );
      return 2;  /* nil and error on the stack */
    }
    success = TRUE;
  }

  luaproc_unlock_channel( chan );

  if ( success ) {
    lua_pushboolean( L, TRUE );
    return 1;
  }

  lua_pushnil( L );
  lua_pushstring( L, "no one receive" );
  return 2;
}

/* create a new channel */
static int luaproc_create_channel (lua_State *L)
{
  const char *chname = luaL_checkstring( L, 1 );

  channel *chan = channel_locked_get( chname );
  if (chan != NULL) {  /* does channel exist? */
    /* unlock the channel mutex locked by channel_locked_get */
    luaproc_unlock_channel( chan );
    /* return an error to lua */
    lua_pushnil( L );
    lua_pushfstring( L, "channel '%s' already exists", chname );
    return 2;
  } else {  /* create channel */
    channel_create( chname );
    lua_pushboolean( L, TRUE );
    return 1;
  }
}

/* destroy a channel */
static int luaproc_destroy_channel (lua_State *L)
{
  const char *chname = luaL_checkstring( L,  1 );

  /* get exclusive access to channels list */
  mtx_lock( &mutex_channel_list );

  /*
     try to get channel and lock it; if lock fails, release external
     lock ('mutex_channel_list') to try again when signaled -- this avoids
     keeping the external lock busy for too long. during this release,
     the channel may have been destroyed, so it must try to get it again.
  */
  channel *chan = NULL;
  while (( chan = channel_unlocked_get( chname )) != NULL
    && mtx_trylock( &chan->mutex ) != 0 )
  {
    cnd_wait( &chan->can_be_used, &mutex_channel_list );
  }

  if ( chan == NULL ) {  /* found channel? */
    /* release exclusive access to channels list */
    mtx_unlock( &mutex_channel_list );
    /* return an error to lua */
    lua_pushnil( L );
    lua_pushfstring( L, "channel '%s' does not exist", chname );
    return 2;
  }

  /* remove channel from table */
  lua_getglobal( chanls, LUAPROC_CHANNELS_TABLE );
  lua_pushnil( chanls );
  lua_setfield( chanls, -2, chname );
  lua_pop( chanls, 1 );

  mtx_unlock( &mutex_channel_list );

  /*
     wake up workers there are waiting to use the channel.
     they will not find the channel, since it was removed,
     and will not get this condition anymore.
   */
  cnd_broadcast( &chan->can_be_used );

  /*
     dequeue lua processes waiting on the channel, return an error message
     to each of them indicating channel was destroyed and schedule them
     for execution (unblock them).
   */
  list *blockedlp = NULL;
  if ( chan->send.head != NULL ) {
    lua_pushfstring( L, "channel '%s' destroyed while waiting for receiver",
      chname );
    blockedlp = &chan->send;
  } else {
    lua_pushfstring( L, "channel '%s' destroyed while waiting for sender",
      chname );
    blockedlp = &chan->recv;
  }
  luaproc *lp = NULL;
  while (( lp = list_remove( blockedlp )) != NULL ) {
    /* return an error to each process */
    lua_pushnil( lp->lstate );
    lua_pushstring( lp->lstate, lua_tostring( L, -1 ));
    lp->args = 2;
    sched_queue_proc( lp ); /* schedule process for execution */
  }

  /* unlock channel mutex and destroy both mutex and condition */
  mtx_unlock( &chan->mutex );
  mtx_destroy( &chan->mutex );
  cnd_destroy( &chan->can_be_used );

  lua_pushboolean( L, TRUE );
  return 1;
}

/***********************
 * get'ers and set'ers *
 ***********************/

/* return the channel where a lua process is blocked at */
channel *luaproc_get_channel (luaproc *lp)
{
  return lp->chan;
}

/* return a lua process' status */
int luaproc_get_status (luaproc *lp)
{
  return lp->status;
}

/* set lua a process' status */
void luaproc_set_status (luaproc *lp, int status)
{
  lp->status = status;
}

/* return a lua process' state */
lua_State *luaproc_get_state (luaproc *lp)
{
  return lp->lstate;
}

/* return the number of arguments expected by a lua process */
int luaproc_get_numargs (luaproc *lp)
{
  return lp->args;
}

/* set the number of arguments expected by a lua process */
void luaproc_set_numargs (luaproc *lp, int n)
{
  lp->args = n;
}

/**********************************
 * register structs and functions *
 **********************************/

static void luaproc_reglualib (
  lua_State *L, const char *name, lua_CFunction f)
{
  lua_getglobal( L, "package" );
  lua_getfield( L, -1, "preload" );
  lua_pushcfunction( L, f );
  lua_setfield( L, -2, name );
  lua_pop( L, 2 );
}

static void luaproc_openlualibs (lua_State *L)
{
  requiref( L, "_G", luaopen_base, FALSE );
  requiref( L, "package", luaopen_package, TRUE );
  luaproc_reglualib( L, "io", luaopen_io );
  luaproc_reglualib( L, "os", luaopen_os );
  luaproc_reglualib( L, "table", luaopen_table );
  luaproc_reglualib( L, "string", luaopen_string );
  luaproc_reglualib( L, "math", luaopen_math );
  luaproc_reglualib( L, "debug", luaopen_debug );
  luaproc_reglualib( L, "coroutine", luaopen_coroutine );
  luaproc_reglualib( L, "utf8", luaopen_utf8 );
}

LUALIB_API int luaopen_luaproc (lua_State *L)
{
  /* register luaproc functions */
  luaL_newlib( L, luaproc_funcs );

  /* thread init */
  mtx_init(&mutex_channel_list, mtx_plain);
  mtx_init(&mutex_recycle_list, mtx_plain);
  mtx_init(&mutex_mainls, mtx_plain);
  cnd_init(&cond_mainls_sendrecv);

  /* wrap main state inside a lua process */
  mainlp.lstate = L;
  mainlp.status = LUAPROC_STATUS_IDLE;
  mainlp.args   = 0;
  mainlp.chan   = NULL;
  mainlp.next   = NULL;
  /* initialize recycle list */
  list_init( &recycle_list );
  /* initialize channels table and lua_State used to store it */
  chanls = luaL_newstate();
  lua_newtable( chanls );
  lua_setglobal( chanls, LUAPROC_CHANNELS_TABLE );
  /* create finalizer to join workers when Lua exits */
  lua_newuserdata( L, 0 );
  lua_setfield( L, LUA_REGISTRYINDEX, "LUAPROC_FINALIZER_UDATA" );
  luaL_newmetatable( L, "LUAPROC_FINALIZER_MT" );
  lua_pushliteral( L, "__gc" );
  lua_pushcfunction( L, luaproc_join_workers );
  lua_rawset( L, -3 );
  lua_pop( L, 1 );
  lua_getfield( L, LUA_REGISTRYINDEX, "LUAPROC_FINALIZER_UDATA" );
  lua_getfield( L, LUA_REGISTRYINDEX, "LUAPROC_FINALIZER_MT" );
  lua_setmetatable( L, -2 );
  lua_pop( L, 1 );
  /* initialize scheduler */
  if ( sched_init() == LUAPROC_SCHED_PTHREAD_ERROR ) {
    luaL_error( L, "failed to create worker" );
  }

  return 1;
}

static int luaproc_loadlib (lua_State *L)
{
  /* register luaproc functions */
  luaL_newlib( L, luaproc_funcs );

  return 1;
}
