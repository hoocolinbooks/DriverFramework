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
#include <pthread.h>
#include "DriverFramework.hpp"
#include "SyncObj.hpp"

#define DEBUG(FMT, ...)
//#define DEBUG(FMT, ...) printf(FMT, __VA_ARGS__)

using namespace DriverFramework;

SyncObj::SyncObj()
{
	m_lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
	m_new_data_cond = (pthread_cond_t)PTHREAD_COND_INITIALIZER;
}

SyncObj::~SyncObj()
{
}

void SyncObj::lock()
{
	DEBUG("Lock %p\n", this);
	pthread_mutex_lock(&m_lock);
}

void SyncObj::unlock()
{
	DEBUG("unock %p\n", this);
	pthread_mutex_unlock(&m_lock);
}

int SyncObj::waitOnSignal(unsigned long timeout_ms)
{
	int ret;
	DEBUG("wait %p\n", &m_new_data_cond);
	if (timeout_ms) {
		struct timespec ts = absoluteTimeInFuture(timeout_ms);
		ret = pthread_cond_timedwait(&m_new_data_cond, &m_lock, &ts);
	}
	else {
		ret = pthread_cond_wait(&m_new_data_cond, &m_lock);
	}

	return ret;
}

void SyncObj::signal(void)
{
	DEBUG("signal %p\n", &m_new_data_cond);
	pthread_cond_signal(&m_new_data_cond);
}

