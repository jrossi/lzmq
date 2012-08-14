#include "lzmq.h"
#include "lzutils.h"
#include "lua.h"
#include "lauxlib.h"
#include <Windows.h>
#include <time.h>
#include <stdio.h>
#include "ztimer.h"
#include <stdint.h>

#if defined(__WINDOWS__) 

// #define USE_TICK_COUNT
// #define USE_TICK_COUNT64
#define USE_PERF_COUNT

#else

#define USE_GETTIMEOFDAY
#define USE_CLOCK_MONOTONIC

#endif

static const char *LUAZMQ_MONOTONIC_TIMER = LUAZMQ_PREFIX "monotonic timer";
static const char *LUAZMQ_ABSULUTE_TIMER  = LUAZMQ_PREFIX "absolute timer";

#define LUAZMQ_FLAG_TIMER_STARTED (uchar)2
#define LUAZMQ_FLAG_TIMER_SETTED  (uchar)4

static int64_t U64Delta(uint64_t s, uint64_t e){
  int64_t diff = (e > s)?(int64_t)(e - s):-(int64_t)(s - e);
  return diff;
}

#if defined(__WINDOWS__) 

#define USE_TICK_COUNT

#ifdef USE_TICK_COUNT

typedef DWORD monotonic_time_t;
typedef DWORD monotonic_diff_t;

static void InitMonotonicTimer(){}

static monotonic_time_t GetMonotonicTime(){
  return GetTickCount();
}

static monotonic_diff_t GetMonotonicDelta(monotonic_time_t StartTime, monotonic_time_t EndTime){
  if(StartTime > EndTime)
    return (MAXDWORD - StartTime) + EndTime;
  return EndTime - StartTime;
}

static monotonic_time_t IncMonotonic(monotonic_time_t StartTime, monotonic_diff_t delta){
  return StartTime + delta;
}

#elif defined(USE_TICK_COUNT64)

typedef uint64_t monotonic_time_t;
typedef int64_t  monotonic_diff_t;

static void InitMonotonicTimer(){}

static monotonic_time_t GetMonotonicTime(){
  return GetTickCount64();
}

static monotonic_diff_t GetMonotonicDelta(monotonic_time_t StartTime, monotonic_time_t EndTime){
  return U64Delta(StartTime, EndTime);
}

static monotonic_time_t IncMonotonic(monotonic_time_t StartTime, monotonic_diff_t delta){
  return StartTime + delta;
}

#elif defined(USE_PERF_COUNT)

static LARGE_INTEGER PerfFreq;

typedef uint64_t monotonic_time_t;
typedef int64_t  monotonic_diff_t;

static void InitMonotonicTimer(){
  QueryPerformanceFrequency(&PerfFreq);
  PerfFreq.QuadPart /= 1000;
}

static monotonic_time_t GetMonotonicTime(){
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

static monotonic_diff_t GetMonotonicDelta(monotonic_time_t StartTime, monotonic_time_t EndTime){
  return U64Delta(StartTime, EndTime)/PerfFreq.QuadPart;
}

static monotonic_time_t IncMonotonic(monotonic_time_t StartTime, monotonic_diff_t delta){
  return StartTime + delta * PerfFreq.QuadPart;
}

#endif

typedef uint64_t absolute_time_t;
typedef int64_t  absolute_diff_t;

absolute_time_t GetUtcTime(){
  FILETIME ft;
  absolute_time_t t;
  GetSystemTimeAsFileTime (&ft);
  t = (absolute_time_t)ft.dwLowDateTime + ((absolute_time_t)(ft.dwHighDateTime-1) << 32LL);
  return (t / 10000UL) + 116444736000000000LL;
}

static absolute_diff_t GetUtcDelta(absolute_time_t StartTime, absolute_time_t EndTime){
  return U64Delta(StartTime, EndTime);
}

#else // not __WINDOWS__

typedef uint64_t absolute_time_t;
typedef int64_t  absolute_diff_t;
typedef uint64_t monotonic_time_t;
typedef int64_t  monotonic_diff_t;


absolute_time_t GetUtcTime(){
#ifdef USE_GETTIMEOFDAY
  struct timeval tv;
  if (0 == gettimeofday(&tv, NULL))
    return absolute_time_t(tv.tv_sec) + tv.tv_usec / (absolute_time_t)(1000000);
#endif
  return time(0);
}

monotonic_time_t GetMonotonicTime(){
#ifdef USE_CLOCK_MONOTONIC
  struct timespec ts;
  if(0 == clock_gettime(CLOCK_MONOTONIC, &ts))
    return monotonic_time_t(tv.tv_sec) + tv.tv_usec / (monotonic_time_t)(1000000000);
#endif
  return GetUtcTime();
}

static absolute_diff_t GetUtcDelta(absolute_time_t StartTime, absolute_time_t EndTime){
  return U64Delta(StartTime, EndTime);
}

static monotonic_diff_t GetMonotonicDelta(monotonic_time_t StartTime, monotonic_time_t EndTime){
  return U64Delta(StartTime, EndTime);
}

#endif // __WINDOWS__

static monotonic_diff_t GetMonotonicElapsed(monotonic_time_t StartTime){
  return GetMonotonicDelta(StartTime, GetMonotonicTime());
}

static absolute_diff_t GetUtcElapsed(absolute_time_t StartTime){
  return GetUtcDelta(StartTime, GetUtcTime());
}

// ?? may be we shuld use upvalue insted copy/past ??

typedef struct {
  monotonic_time_t start;
  monotonic_diff_t fire; // interval
  uchar flags;
} zmonotonic_timer;

typedef struct {
  absolute_time_t start;
  absolute_time_t fire; // time point
  uchar flags;
} zabsolute_timer;


zmonotonic_timer *luazmq_getmontimer_at (lua_State *L, int i) {
 zmonotonic_timer *timer = (zmonotonic_timer *)luazmq_checkudatap (L, i, LUAZMQ_MONOTONIC_TIMER);
 luaL_argcheck (L, timer != NULL, 1, LUAZMQ_PREFIX"timer expected");
 luaL_argcheck (L, !(timer->flags & LUAZMQ_FLAG_CLOSED), 1, LUAZMQ_PREFIX"timer is closed");
 return timer;
}
#define luazmq_getmontimer(L) luazmq_getmontimer_at((L),1)

zabsolute_timer *luazmq_getabstimer_at (lua_State *L, int i) {
 zabsolute_timer *timer = (zabsolute_timer *)luazmq_checkudatap (L, i, LUAZMQ_ABSULUTE_TIMER);
 luaL_argcheck (L, timer != NULL, 1, LUAZMQ_PREFIX"timer expected");
 luaL_argcheck (L, !(timer->flags & LUAZMQ_FLAG_CLOSED), 1, LUAZMQ_PREFIX"timer is closed");
 return timer;
}
#define luazmq_getabstimer(L) luazmq_getabstimer_at((L),1)

//-----------------------------------------------------------  
// monotonic
//{----------------------------------------------------------

static int luazmq_timer_create_monotonic(lua_State *L){
  zmonotonic_timer *timer = luazmq_newudata(L, zmonotonic_timer, LUAZMQ_MONOTONIC_TIMER);
  if(lua_isnumber(L, 1)){
    timer->fire   = (monotonic_diff_t)lua_tonumber(L, 1);
    timer->flags |= LUAZMQ_FLAG_TIMER_SETTED;
  }
  return 1;
}

static int luazmq_montimer_close(lua_State *L){
  zmonotonic_timer *timer = luazmq_checkudatap(L, 1, LUAZMQ_MONOTONIC_TIMER);
  luaL_argcheck (L, timer != NULL, 1, LUAZMQ_PREFIX"timer expected");
  if(!(timer->flags & LUAZMQ_FLAG_CLOSED)){
    timer->flags |= LUAZMQ_FLAG_CLOSED;
  }
  return luazmq_pass(L);
}

static int luazmq_montimer_closed(lua_State *L){
  zmonotonic_timer *timer = luazmq_checkudatap(L, 1, LUAZMQ_MONOTONIC_TIMER);
  luaL_argcheck (L, timer != NULL, 1, LUAZMQ_PREFIX"timer expected");
  lua_pushboolean(L, timer->flags & LUAZMQ_FLAG_CLOSED);
  return 1;
}

static int luazmq_montimer_start(lua_State *L){
  zmonotonic_timer *timer = luazmq_getmontimer(L);
  // luaL_argcheck (L, !(timer->flags & LUAZMQ_FLAG_TIMER_STARTED), 1, LUAZMQ_PREFIX"timer already started");
  timer->start  = GetMonotonicTime();
  timer->flags |= LUAZMQ_FLAG_TIMER_STARTED;
  if(lua_isnumber(L, 2)){
    timer->fire   = (monotonic_diff_t)lua_tonumber(L, 2);
    timer->flags |= LUAZMQ_FLAG_TIMER_SETTED;
  }
  return 1;
}

static int luazmq_montimer_started(lua_State *L){
  zmonotonic_timer *timer = luazmq_getmontimer(L);
  lua_pushboolean(L, timer->flags & LUAZMQ_FLAG_TIMER_STARTED);
  return 1;
}

static int luazmq_montimer_elapsed(lua_State *L){
  zmonotonic_timer *timer = luazmq_getmontimer(L);
  monotonic_diff_t elapsed;
  luaL_argcheck (L, (timer->flags & LUAZMQ_FLAG_TIMER_STARTED), 1, LUAZMQ_PREFIX"timer not started");
  elapsed = GetMonotonicElapsed(timer->start);
  lua_pushnumber(L, elapsed);
  return 1;
}

static int luazmq_montimer_rest(lua_State *L){
  zmonotonic_timer *timer = luazmq_getmontimer(L);
  monotonic_diff_t elapsed;
  luaL_argcheck (L, (timer->flags & LUAZMQ_FLAG_TIMER_STARTED), 1, LUAZMQ_PREFIX"timer not started");
  luaL_argcheck (L, (timer->flags & LUAZMQ_FLAG_TIMER_SETTED), 1, LUAZMQ_PREFIX"timer not setted");

  elapsed = GetMonotonicElapsed(timer->start);
  if(elapsed >= timer->fire) lua_pushinteger(L, 0);
  else lua_pushnumber(L, timer->fire - elapsed);

  return 1;
}

static int luazmq_montimer_set(lua_State *L){
  zmonotonic_timer *timer = luazmq_getmontimer(L);
  timer->fire   = (monotonic_diff_t)luaL_checknumber(L, 2);
  timer->flags |= LUAZMQ_FLAG_TIMER_SETTED;
  return 1;
}

static int luazmq_montimer_get (lua_State *L){
  zmonotonic_timer *timer = luazmq_getmontimer(L);
  if(timer->flags & LUAZMQ_FLAG_TIMER_SETTED) 
    lua_pushnumber(L,  timer->fire);
  else 
    lua_pushnil(L);
  return 1;
}

static int luazmq_montimer_setted(lua_State *L){
  zmonotonic_timer *timer = luazmq_getmontimer(L);
  lua_pushboolean(L, timer->flags & LUAZMQ_FLAG_TIMER_SETTED);
  return 1;
}

static int luazmq_montimer_reset(lua_State *L){
  zmonotonic_timer *timer = luazmq_getmontimer(L);
  timer->flags &= ~LUAZMQ_FLAG_TIMER_SETTED;
  return luazmq_pass(L);
}

static int luazmq_montimer_stop(lua_State *L){
  zmonotonic_timer *timer = luazmq_getmontimer(L);
  monotonic_diff_t elapsed;
  luaL_argcheck (L, (timer->flags & LUAZMQ_FLAG_TIMER_STARTED), 1, LUAZMQ_PREFIX"timer not started");
  elapsed = GetMonotonicElapsed(timer->start);
  lua_pushnumber(L, elapsed);
  timer->flags &= ~LUAZMQ_FLAG_TIMER_STARTED;
  return 1;
}

static int luazmq_montimer_is_absolute(lua_State *L){
  luazmq_getmontimer(L);
  lua_pushboolean(L, 0);
  return 1;
}

static int luazmq_montimer_is_monotonic(lua_State *L){
  luazmq_getmontimer(L);
  lua_pushboolean(L, 1);
  return 1;
}

//}----------------------------------------------------------

//-----------------------------------------------------------  
// absolute
//{----------------------------------------------------------

static int luazmq_timer_create_absolute(lua_State *L){
  zabsolute_timer *timer = luazmq_newudata(L, zabsolute_timer, LUAZMQ_ABSULUTE_TIMER);
  if(lua_isnumber(L, 1)){
    timer->fire   = (absolute_time_t)lua_tonumber(L, 1);
    timer->flags |= LUAZMQ_FLAG_TIMER_SETTED;
  }
  return 1;
}

static int luazmq_abstimer_close(lua_State *L){
  zabsolute_timer *timer = luazmq_checkudatap(L, 1, LUAZMQ_ABSULUTE_TIMER);
  luaL_argcheck (L, timer != NULL, 1, LUAZMQ_PREFIX"timer expected");
  if(!(timer->flags & LUAZMQ_FLAG_CLOSED)){
    timer->flags |= LUAZMQ_FLAG_CLOSED;
  }
  return luazmq_pass(L);
}

static int luazmq_abstimer_closed(lua_State *L){
  zabsolute_timer *timer = luazmq_checkudatap(L, 1, LUAZMQ_ABSULUTE_TIMER);
  luaL_argcheck (L, timer != NULL, 1, LUAZMQ_PREFIX"timer expected");
  lua_pushboolean(L, timer->flags & LUAZMQ_FLAG_CLOSED);
  return 1;
}

static int luazmq_abstimer_start(lua_State *L){
  zabsolute_timer *timer = luazmq_getabstimer(L);
  // luaL_argcheck (L, !(timer->flags & LUAZMQ_FLAG_TIMER_STARTED), 1, LUAZMQ_PREFIX"timer already started");
  timer->start  = GetUtcTime();
  timer->flags |= LUAZMQ_FLAG_TIMER_STARTED;
  if(lua_isnumber(L, 2)){
    timer->fire   = (absolute_time_t)lua_tonumber(L, 2);
    timer->flags |= LUAZMQ_FLAG_TIMER_SETTED;
  }
  return 1;
}

static int luazmq_abstimer_started(lua_State *L){
  zabsolute_timer *timer = luazmq_getabstimer(L);
  lua_pushboolean(L, timer->flags & LUAZMQ_FLAG_TIMER_STARTED);
  return 1;
}

static int luazmq_abstimer_elapsed(lua_State *L){
  zabsolute_timer *timer = luazmq_getabstimer(L);
  absolute_diff_t elapsed;
  luaL_argcheck (L, (timer->flags & LUAZMQ_FLAG_TIMER_STARTED), 1, LUAZMQ_PREFIX"timer not started");
  elapsed = GetUtcElapsed(timer->start);
  lua_pushnumber(L, (lua_Number)elapsed);
  return 1;
}

static int luazmq_abstimer_rest(lua_State *L){
  zabsolute_timer *timer = luazmq_getabstimer(L);
  absolute_diff_t rest;
  // do we need start?
  luaL_argcheck (L, (timer->flags & LUAZMQ_FLAG_TIMER_STARTED), 1, LUAZMQ_PREFIX"timer not started");
  luaL_argcheck (L, (timer->flags & LUAZMQ_FLAG_TIMER_SETTED), 1, LUAZMQ_PREFIX"timer not setted");

  rest = GetUtcDelta(GetUtcTime(), timer->fire);
  if(rest <= 0) lua_pushinteger(L, 0);
  else lua_pushnumber(L, (lua_Number)rest);

  return 1;
}

static int luazmq_abstimer_set(lua_State *L){
  zabsolute_timer *timer = luazmq_getabstimer(L);
  timer->fire   = (absolute_time_t)luaL_checknumber(L, 2);
  timer->flags |= LUAZMQ_FLAG_TIMER_SETTED;
  return 1;
}

static int luazmq_abstimer_get (lua_State *L){
  zabsolute_timer *timer = luazmq_getabstimer(L);
  if(timer->flags & LUAZMQ_FLAG_TIMER_SETTED) 
    lua_pushnumber(L,  (lua_Number)timer->fire);
  else 
    lua_pushnil(L);
  return 1;
}

static int luazmq_abstimer_setted(lua_State *L){
  zabsolute_timer *timer = luazmq_getabstimer(L);
  lua_pushboolean(L, timer->flags & LUAZMQ_FLAG_TIMER_SETTED);
  return 1;
}

static int luazmq_abstimer_reset(lua_State *L){
  zabsolute_timer *timer = luazmq_getabstimer(L);
  timer->flags &= ~LUAZMQ_FLAG_TIMER_SETTED;
  return luazmq_pass(L);
}

static int luazmq_abstimer_stop(lua_State *L){
  zabsolute_timer *timer = luazmq_getabstimer(L);
  absolute_diff_t elapsed;
  luaL_argcheck (L, (timer->flags & LUAZMQ_FLAG_TIMER_STARTED), 1, LUAZMQ_PREFIX"timer not started");
  elapsed = GetUtcElapsed(timer->start);
  lua_pushnumber(L, (lua_Number)elapsed);
  timer->flags &= ~LUAZMQ_FLAG_TIMER_STARTED;
  return 1;
}

static int luazmq_abstimer_is_absolute(lua_State *L){
  luazmq_getabstimer(L);
  lua_pushboolean(L, 1);
  return 1;
}

static int luazmq_abstimer_is_monotonic(lua_State *L){
  luazmq_getabstimer(L);
  lua_pushboolean(L, 0);
  return 1;
}

//}----------------------------------------------------------

static int luazmq_timer_absolute_time(lua_State *L){
  lua_pushnumber(L, (lua_Number)GetUtcTime());
  return 1;
}

static int luazmq_timer_sleep(lua_State *L){
  int msecs = luaL_checkint(L,1);

#if defined (__WINDOWS__)
// Windows XP/2000: A value of zero causes the thread to relinquish the
// remainder of its time slice to any other thread of equal priority that is
// ready to run. If there are no other threads of equal priority ready to run,
// the function returns immediately, and the thread continues execution. This
// behavior changed starting with Windows Server 2003.
# if defined (NTDDI_VERSION) && defined (NTDDI_WS03) && (NTDDI_VERSION >= NTDDI_WS03)
    Sleep (msecs);
# else
    if (msecs > 0)
        Sleep (msecs);
# endif
#else
    struct timespec t;
    t.tv_sec = msecs / 1000;
    t.tv_nsec = (msecs % 1000) * 1000000;
    nanosleep (&t, NULL);
#endif

  return luazmq_pass(L);
}

static const struct luaL_Reg luazmq_timerlib[]   = {
  { "monotonic",     luazmq_timer_create_monotonic },
  { "absolute",      luazmq_timer_create_absolute  },
  { "absolute_time", luazmq_timer_absolute_time    },
  { "sleep",         luazmq_timer_sleep            },

  {NULL, NULL}
};

static const struct luaL_Reg luazmq_montimer_methods[] = {
  { "__gc",         luazmq_montimer_close        },
  { "close",        luazmq_montimer_close        },
  { "closed",       luazmq_montimer_closed       },
  { "set",          luazmq_montimer_set          },
  { "get",          luazmq_montimer_get          },
  { "reset",        luazmq_montimer_reset        },
  { "setted",       luazmq_montimer_setted       },
  { "start",        luazmq_montimer_start        },
  { "started",      luazmq_montimer_started      },
  { "elapsed",      luazmq_montimer_elapsed      },
  { "rest",         luazmq_montimer_rest         },
  { "stop",         luazmq_montimer_stop         },
  { "is_absolute",  luazmq_montimer_is_absolute  }, 
  { "is_monotonic", luazmq_montimer_is_monotonic },

  {NULL,NULL}
};

static const struct luaL_Reg luazmq_abstimer_methods[] = {
  { "__gc",         luazmq_abstimer_close        },
  { "close",        luazmq_abstimer_close        },
  { "closed",       luazmq_abstimer_closed       },
  { "set",          luazmq_abstimer_set          },
  { "get",          luazmq_abstimer_get          },
  { "reset",        luazmq_abstimer_reset        },
  { "setted",       luazmq_abstimer_setted       },
  { "start",        luazmq_abstimer_start        },
  { "started",      luazmq_abstimer_started      },
  { "elapsed",      luazmq_abstimer_elapsed      },
  { "rest",         luazmq_abstimer_rest         },
  { "stop",         luazmq_abstimer_stop         },
  { "is_absolute",  luazmq_abstimer_is_absolute  }, 
  { "is_monotonic", luazmq_abstimer_is_monotonic },

  {NULL,NULL}
};

void luazmq_timer_initlib(lua_State *L){
  InitMonotonicTimer();
  luazmq_createmeta(L, LUAZMQ_MONOTONIC_TIMER,  luazmq_montimer_methods);
  luazmq_createmeta(L, LUAZMQ_ABSULUTE_TIMER,   luazmq_abstimer_methods);
  lua_pop(L, 2);

  lua_newtable(L);
  luazmq_setfuncs(L, luazmq_timerlib, 0);
  lua_setfield(L,-2, "timer");
}
