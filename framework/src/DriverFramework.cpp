/**********************************************************************
* Copyright (c) 2015 Mark Charlebois
* 
* All rights reserved.
* 
* Redistribution and use in source and binary forms, with or without
* modification, are permitted (subject to the limitations in the
* disclaimer below) provided that the following conditions are met:
* 
*  * Redistributions of source code must retain the above copyright
*    notice, this list of conditions and the following disclaimer.
* 
*  * Redistributions in binary form must reproduce the above copyright
*    notice, this list of conditions and the following disclaimer in the
*    documentation and/or other materials provided with the
*    distribution.
* 
*  * Neither the name of Dronecode Project nor the names of its
*    contributors may be used to endorse or promote products derived
*    from this software without specific prior written permission.
* 
* NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
* GRANTED BY THIS LICENSE.  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
* HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
* WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
* LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
* CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
* SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
* BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
* WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
* OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
* IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*************************************************************************/
#include <stdio.h>
#include <list>
#include <map>
#include <pthread.h>
#include "DriverFramework.hpp"
#include "DevObj.hpp"
#include "DevMgr.hpp"

// Used for backtrace
#ifdef DF_ENABLE_BACKTRACE
#include <stdlib.h>
#include <execinfo.h>
#endif

#if defined(__APPLE__) && defined(__MACH__)
#include <mach/mach_time.h>
#define MAC_NANO (+1.0E-9)
#define MAC_GIGA (UINT64_C(1000000000))
#endif

#define SHOW_STATS 0

using namespace DriverFramework;

//-----------------------------------------------------------------------
// Types
//-----------------------------------------------------------------------
class WorkItem
{
public:
	WorkItem(workCallback callback, void *arg, uint32_t delay, WorkHandle handle) : 
		m_arg(arg),
		m_queue_time(0),
		m_callback(callback),
		m_delay(delay),
		m_handle(handle)
	{
		resetStats();
	}
	~WorkItem() {}

	void schedule();

	void updateStats(unsigned int cur_usec);
	void resetStats();
	void dumpStats();

	void *		m_arg;
	uint64_t	m_queue_time;
	workCallback	m_callback;
	uint32_t	m_delay;
	WorkHandle	m_handle;

	// statistics
	unsigned long m_last;
	unsigned long m_min;
	unsigned long m_max;
	unsigned long m_total;
	unsigned long m_count;
};

class HRTWorkQueue
{
public:
	static HRTWorkQueue *instance(void);

	static int initialize(void);
	static void finalize(void);

	void scheduleWorkItem(WorkItem *item);

	void shutdown(void);
	void enableStats(bool enable);
	void clearAll();

	static void *process_trampoline(void *);

private:
	HRTWorkQueue(void) {}
	~HRTWorkQueue(void) {}

	void process(void);

	void hrtLock(void);
	void hrtUnlock(void);

	std::list<WorkItem *>	m_work;

	bool m_enable_stats = false;
	bool m_exit_requested = false;

	static HRTWorkQueue *m_instance;
};

//-----------------------------------------------------------------------
// Static Variables
//-----------------------------------------------------------------------

static uint64_t g_timestart = 0;
static pthread_t g_tid;

static pthread_mutex_t g_framework_exit = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_hrt_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_reschedule_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_framework_cond = PTHREAD_COND_INITIALIZER;

static std::map<WorkHandle, WorkItem> *g_work_items = nullptr;

//-----------------------------------------------------------------------
// Static Functions
//-----------------------------------------------------------------------

static uint64_t TSToABSTime(struct timespec *ts)
{
        uint64_t result;

        result = (uint64_t)(ts->tv_sec) * 1000000;
        result += ts->tv_nsec / 1000;

        return result;
}

int DriverFramework::clockGetRealtime(struct timespec *ts)
{
#if defined(__APPLE__) && defined(__MACH__)

	mach_timebase_info_data_t tb = {};
	mach_timebase_info(&tb);
	double px4_timebase = tb.numer;
	px4_timebase /= tb.denom;

	double diff = mach_absolute_time() * px4_timebase;
	ts->tv_sec = diff * MAC_NANO;
	ts->tv_nsec = diff - (ts->tv_sec * MAC_GIGA);

	return 0;

#else

	return clock_gettime(CLOCK_REALTIME, ts);

#endif
}

//-----------------------------------------------------------------------
// Global Functions
//-----------------------------------------------------------------------
uint64_t DriverFramework::offsetTime(void)
{
	struct timespec ts = {};

	(void)clockGetRealtime(&ts);

	if (!g_timestart) {
		g_timestart = TSToABSTime(&ts);
	}

	// Time is in microseconds
	return TSToABSTime(&ts) - g_timestart;
}

timespec DriverFramework::offsetTimeToAbsoluteTime(uint64_t offset_time)
{
	uint64_t abs_time = offset_time + g_timestart;
	struct timespec ts = {};
	ts.tv_sec = abs_time / 1000000;
	ts.tv_nsec = (abs_time % 1000000) * 1000;

	return ts;
}

timespec DriverFramework::absoluteTimeInFuture(uint64_t time_ms)
{
	struct timespec ts;

	clockGetRealtime(&ts);

	uint64_t nsecs = ts.tv_nsec + time_ms*1000000;
	uint64_t secs = (nsecs/1000000000);
	
	ts.tv_sec += secs;
	ts.tv_nsec = nsecs - secs*1000000000;

	return ts;
}

void DriverFramework::backtrace()
{
	void *buffer[10];
	char **callstack;
	int bt_size;
	int idx;

	bt_size = ::backtrace(buffer, 10);
	callstack = ::backtrace_symbols(buffer, bt_size);

	DF_LOG_INFO("Backtrace: %d", bt_size);

	for (idx = 0; idx < bt_size; idx++) {
		DF_LOG_INFO("%s", callstack[idx]);
	}

	free(callstack);
}

//-----------------------------------------------------------------------
// Class Methods
//-----------------------------------------------------------------------

/*************************************************************************
  Framework
*************************************************************************/
void Framework::shutdown()
{
	// Stop the HRT queue thread
	HRTWorkQueue *wq = HRTWorkQueue::instance();
	if (wq) {
		wq->shutdown();
	}

	// Free the HRTWorkQueue resources
	HRTWorkQueue::finalize();

	// Free the WorkMgr resources
	WorkMgr::finalize();

	// Free the DevMgr resources
	DevMgr::finalize();

	// allow Framework to exit
	pthread_mutex_lock(&g_framework_exit);
	pthread_cond_signal(&g_framework_cond);
	pthread_mutex_unlock(&g_framework_exit);
}

int Framework::initialize()
{
	int ret = HRTWorkQueue::initialize();
	if (ret < 0) {
		return ret;
	}
	ret = DevMgr::initialize();
	if (ret < 0) {
		return ret-10;
	}
	ret = WorkMgr::initialize();
	if (ret < 0) {
		return ret-20;
	}
	return 0;
}

void Framework::waitForShutdown()
{
	// Block until shutdown requested
	pthread_mutex_lock(&g_framework_exit);
	pthread_cond_wait(&g_framework_cond, &g_framework_exit);
	pthread_mutex_unlock(&g_framework_exit);
}

/*************************************************************************
  WorkItem
*************************************************************************/
void WorkItem::updateStats(unsigned int cur_usec)
{
	unsigned long delay = (m_last == ~0x0UL) ? (cur_usec - m_queue_time) : (cur_usec - m_last);

	if (delay < m_min) {
		m_min = delay;
	}
	if (delay > m_max) {
		m_max = delay;
	}

	m_total += delay;
	m_count += 1;
	m_last = cur_usec;

#if SHOW_STATS == 1
	if ((m_count % 100) == 99) {
		dumpStats();
	}
#endif
}

void WorkItem::resetStats() 
{
	m_last = ~(uint64_t)0;
	m_min = ~(uint64_t)0;
	m_max = 0;
	m_total = 0;
	m_count = 0;
}

void WorkItem::dumpStats() 
{
	DF_LOG_INFO("Stats for id=%d callback=%p: count=%lu, avg=%lu min=%lu max=%lu\n", 
		m_handle, m_callback, m_count, m_total/m_count, m_min, m_max);
}

/*************************************************************************
  HRTWorkQueue
*************************************************************************/
HRTWorkQueue *HRTWorkQueue::m_instance = NULL;

void *HRTWorkQueue::process_trampoline(void *arg)
{
	if (m_instance) {
		m_instance->process();
	}
	return NULL;
}

HRTWorkQueue *HRTWorkQueue::instance(void)
{
	return m_instance;
}

static int setRealtimeSched(pthread_attr_t &attr)
{
	sched_param param;

	int ret = pthread_attr_init (&attr);
	ret = (!ret) ? ret : pthread_attr_getschedparam (&attr, &param);

	param.sched_priority = sched_get_priority_max(SCHED_FIFO);

	ret = (!ret) ? ret : pthread_attr_setschedparam (&attr, &param);
	ret = (!ret) ? ret : pthread_attr_setinheritsched (&attr, PTHREAD_EXPLICIT_SCHED);
	return ret;
}

int HRTWorkQueue::initialize(void)
{
	m_instance = new HRTWorkQueue();

	if (m_instance == nullptr) {
		return 1;
	}

	// Create a lock for handling the work queue
	if (pthread_mutex_init(&g_hrt_lock, NULL) != 0) {
		return 2;
	}

	pthread_attr_t attr;
	if(setRealtimeSched(attr)) {
		return 3;
	}

	// Create high priority worker thread
	if (pthread_create(&g_tid, &attr, process_trampoline, NULL)) {
		return 4;
	}
	return 0;
}

void HRTWorkQueue::finalize(void)
{
	pthread_join(g_tid, NULL);
	HRTWorkQueue *wq = HRTWorkQueue::instance();
	if (wq) {
		wq->clearAll();
		pthread_mutex_destroy(&g_hrt_lock);

		delete wq;
		wq = nullptr;
	}
}

void HRTWorkQueue::scheduleWorkItem(WorkItem *item)
{
	hrtLock();
	m_work.push_back(item);
	item->m_queue_time = offsetTime();
	pthread_cond_signal(&g_reschedule_cond);
	hrtUnlock();
}

void HRTWorkQueue::clearAll()
{
	hrtLock();
	m_work.clear();
	hrtUnlock();
}

void HRTWorkQueue::shutdown(void)
{
	hrtLock();
	m_exit_requested = true;
	pthread_cond_signal(&g_reschedule_cond);
	hrtUnlock();
}

void HRTWorkQueue::process(void)
{
	std::list<WorkItem *>::iterator work_itr;
	uint64_t next;
	uint64_t elapsed;
	uint64_t remaining;
	timespec ts;
	uint64_t now;

	while(!m_exit_requested) {
		hrtLock();

		// Wake up every 10 sec if nothing scheduled
		next = 10000000;
		work_itr = m_work.begin();

		now = offsetTime();
		while (work_itr != m_work.end()) {
			now = offsetTime();
			elapsed = now - (*work_itr)->m_queue_time;

			if (elapsed >= (*work_itr)->m_delay) {
				WorkItem *dequeuedWork = *work_itr;

				// Mutex is recursive so the WorkItem can be rescheduled
				// while the lock is held
				dequeuedWork->updateStats(now);
				hrtUnlock();
				dequeuedWork->m_callback(dequeuedWork->m_arg, dequeuedWork->m_handle);
				hrtLock();

				work_itr = m_work.erase(work_itr);
			} else {
				remaining = (*work_itr)->m_delay - elapsed;
				if (remaining < next) {
					next = remaining;
				}

				// try the next in the list
				++work_itr;
			}
		}


		// pthread_cond_timedwait uses absolute time
		ts = offsetTimeToAbsoluteTime(now+next);
		
		// Wait until next expiry or until a new item is rescheduled
		pthread_cond_timedwait(&g_reschedule_cond, &g_hrt_lock, &ts);
		hrtUnlock();
	}
}

void HRTWorkQueue::hrtLock()
{
	pthread_mutex_lock(&g_hrt_lock);
}

void HRTWorkQueue::hrtUnlock()
{
	pthread_mutex_unlock(&g_hrt_lock);
}

/*************************************************************************
  WorkMgr
*************************************************************************/
int WorkMgr::initialize()
{
	g_work_items = new std::map<WorkHandle,WorkItem>;
	return 0;
}

void WorkMgr::finalize()
{
	std::map<WorkHandle,WorkItem>::iterator it = g_work_items->begin();
	while(it != g_work_items->end())
	{
		// Remove the element from the map
		g_work_items->erase(it);
		it = g_work_items->begin();
	}
	delete g_work_items;
	g_work_items = nullptr;
}

WorkHandle WorkMgr::create(workCallback cb, void *arg, uint32_t delay)
{
	static WorkHandle i=1000;
	std::map<WorkHandle,WorkItem>::iterator it = g_work_items->begin();
	++i;
  	g_work_items->insert (it, std::pair<WorkHandle,WorkItem>(i, WorkItem(cb, arg, delay, i)));
	return i;
}

void WorkMgr::destroy(WorkHandle &handle)
{
	// remove from map, then from work queue, then delete
	std::map<WorkHandle,WorkItem>::iterator it = g_work_items->find(handle);
	if (it != g_work_items->end()) {
		g_work_items->erase(it);
	}
	// mark the handle as cleared
	handle = 0;
}

bool WorkMgr::schedule(WorkHandle handle)
{
	std::map<WorkHandle,WorkItem>::iterator it = g_work_items->find(handle);
	bool ret = it != g_work_items->end();
	if (ret) {
		HRTWorkQueue::instance()->scheduleWorkItem(&(it->second));
	}
	return ret;
}

