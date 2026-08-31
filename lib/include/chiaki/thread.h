// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

#ifndef CHIAKI_THREAD_H
#define CHIAKI_THREAD_H

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef void *(*ChiakiThreadFunc)(void *);

typedef struct chiaki_thread_t
{
#ifdef _WIN32
	HANDLE thread;
	ChiakiThreadFunc func;
	void *arg;
	void *ret;
#else
	pthread_t thread;
#endif
} ChiakiThread;

CHIAKI_EXPORT ChiakiErrorCode chiaki_thread_create(ChiakiThread *thread, ChiakiThreadFunc func, void *arg);
CHIAKI_EXPORT ChiakiErrorCode chiaki_thread_join(ChiakiThread *thread, void **retval);
CHIAKI_EXPORT ChiakiErrorCode chiaki_thread_timedjoin(ChiakiThread *thread, void **retval, uint64_t timeout_ms);
CHIAKI_EXPORT ChiakiErrorCode chiaki_thread_set_name(ChiakiThread *thread, const char *name);

#ifdef __SWITCH__
// Diagnostic only: logs current/limit for the kernel resource limit
// categories homebrew commonly runs out of (threads, events, sessions,
// transfer memory), via a raw printf so it shows up over nxlink regardless
// of chiaki log level. tag is printed alongside so multiple call sites are
// distinguishable in the log.
void chiaki_switch_log_resource_limits(const char *tag);

// Diagnostic only: registers the calling thread (by its own handle - must
// be called from within the thread being registered) under a fixed, static
// label for periodic per-thread CPU tick sampling. Returns the assigned
// index (or -1 if the table is full) - the caller must hang onto it and
// pass it to chiaki_switch_self_report_ticks() periodically from within its
// own loop, since InfoType_ThreadTickCount can only be queried by a thread
// about itself (verified against a from-scratch kernel reimplementation -
// any other combination silently returns 0 with no error). See thread.c.
#define CHIAKI_SWITCH_MAX_SAMPLED_THREADS 16
typedef struct { const char *label; uint64_t handle; uint64_t self_ticks; } ChiakiSwitchSampledThread;
extern ChiakiSwitchSampledThread chiaki_switch_sampled_threads[CHIAKI_SWITCH_MAX_SAMPLED_THREADS];
extern volatile int chiaki_switch_sampled_threads_count;
int chiaki_switch_register_thread_for_sampling(const char *label);
void chiaki_switch_self_report_ticks(int idx);
#endif


typedef struct chiaki_mutex_t
{
#ifdef _WIN32
	CRITICAL_SECTION cs;
#else
	pthread_mutex_t mutex;
#endif
} ChiakiMutex;

CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_init(ChiakiMutex *mutex, bool rec);
CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_fini(ChiakiMutex *mutex);
CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_lock(ChiakiMutex *mutex);
CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_trylock(ChiakiMutex *mutex);
CHIAKI_EXPORT ChiakiErrorCode chiaki_mutex_unlock(ChiakiMutex *mutex);


typedef struct chiaki_cond_t
{
#ifdef _WIN32
	CONDITION_VARIABLE cond;
#else
	pthread_cond_t cond;
#endif
} ChiakiCond;

typedef bool (*ChiakiCheckPred)(void *);

CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_init(ChiakiCond *cond);
CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_fini(ChiakiCond *cond);
CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_wait(ChiakiCond *cond, ChiakiMutex *mutex);
CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_timedwait(ChiakiCond *cond, ChiakiMutex *mutex, uint64_t timeout_ms);
CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_wait_pred(ChiakiCond *cond, ChiakiMutex *mutex, ChiakiCheckPred check_pred, void *check_pred_user);
CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_timedwait_pred(ChiakiCond *cond, ChiakiMutex *mutex, uint64_t timeout_ms, ChiakiCheckPred check_pred, void *check_pred_user);
CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_signal(ChiakiCond *cond);
CHIAKI_EXPORT ChiakiErrorCode chiaki_cond_broadcast(ChiakiCond *cond);


typedef struct chiaki_bool_pred_cond_t
{
	ChiakiCond cond;
	ChiakiMutex mutex;
	bool pred;
} ChiakiBoolPredCond;

CHIAKI_EXPORT ChiakiErrorCode chiaki_bool_pred_cond_init(ChiakiBoolPredCond *cond);
CHIAKI_EXPORT ChiakiErrorCode chiaki_bool_pred_cond_fini(ChiakiBoolPredCond *cond);
CHIAKI_EXPORT ChiakiErrorCode chiaki_bool_pred_cond_lock(ChiakiBoolPredCond *cond);
CHIAKI_EXPORT ChiakiErrorCode chiaki_bool_pred_cond_unlock(ChiakiBoolPredCond *cond);
CHIAKI_EXPORT ChiakiErrorCode chiaki_bool_pred_cond_wait(ChiakiBoolPredCond *cond);
CHIAKI_EXPORT ChiakiErrorCode chiaki_bool_pred_cond_timedwait(ChiakiBoolPredCond *cond, uint64_t timeout_ms);
CHIAKI_EXPORT ChiakiErrorCode chiaki_bool_pred_cond_signal(ChiakiBoolPredCond *cond);
CHIAKI_EXPORT ChiakiErrorCode chiaki_bool_pred_cond_broadcast(ChiakiBoolPredCond *cond);

#ifdef __cplusplus
}

class ChiakiMutexLock
{
	private:
		ChiakiMutex * const mutex;

	public:
		ChiakiMutexLock(ChiakiMutex *mutex) : mutex(mutex) { chiaki_mutex_lock(mutex); }
		~ChiakiMutexLock() { chiaki_mutex_unlock(mutex); }
};
#endif

#endif // CHIAKI_THREAD_H
