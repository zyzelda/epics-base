/*************************************************************************\
* Copyright (c) 2008 UChicago Argonne LLC, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution. 
\*************************************************************************/
/* callback.c */

/* general purpose callback tasks		*/
/*
 *      Original Author:        Marty Kraimer
 *      Date:   	        07-18-91
*/

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#include "cantProceed.h"
#include "dbDefs.h"
#include "epicsEvent.h"
#include "epicsThread.h"
#include "epicsExit.h"
#include "epicsInterrupt.h"
#include "epicsTimer.h"
#include "epicsRingPointer.h"
#include "errlog.h"
#include "dbStaticLib.h"
#include "dbBase.h"
#include "link.h"
#include "dbFldTypes.h"
#include "recSup.h"
#include "taskwd.h"
#include "errMdef.h"
#include "dbCommon.h"
#define epicsExportSharedSymbols
#include "dbAddr.h"
#include "dbAccessDefs.h"
#include "dbLock.h"
#include "callback.h"


static epicsThreadOnceId callbackOnceFlag = EPICS_THREAD_ONCE_INIT;
static int callbackQueueSize = 2000;
static epicsEventId callbackSem[NUM_CALLBACK_PRIORITIES];
static epicsRingPointerId callbackQ[NUM_CALLBACK_PRIORITIES];
static volatile int ringOverflow[NUM_CALLBACK_PRIORITIES];

/* Timer for Delayed Requests */
static epicsTimerQueueId timerQueue;

/* Shutdown handling */
static epicsEventId exitEvent;
static void *exitValue;

/* Static data */
static char *threadName[NUM_CALLBACK_PRIORITIES] = {
    "cbLow", "cbMedium", "cbHigh"
};
static unsigned int threadPriority[NUM_CALLBACK_PRIORITIES] = {
    epicsThreadPriorityScanLow - 1,
    epicsThreadPriorityScanLow + 4,
    epicsThreadPriorityScanHigh + 1
};
static int priorityValue[NUM_CALLBACK_PRIORITIES] = {0, 1, 2};


int callbackSetQueueSize(int size)
{
    if (callbackOnceFlag != EPICS_THREAD_ONCE_INIT) {
        errlogPrintf("Callback system already initialized\n");
        return -1;
    }
    callbackQueueSize = size;
    return 0;
}

static void callbackTask(void *arg)
{
    int priority = *(int *)arg;

    taskwdInsert(epicsThreadGetIdSelf(), NULL, NULL);
    while(TRUE) {
        epicsEventMustWait(callbackSem[priority]);
        void *ptr;
        while((ptr = epicsRingPointerPop(callbackQ[priority]))) {
            if (ptr == &exitValue) goto shutdown;
            CALLBACK *pcallback = (CALLBACK *)ptr;
            ringOverflow[priority] = FALSE;
            (*pcallback->callback)(pcallback);
        }
    }
shutdown:
    taskwdRemove(epicsThreadGetIdSelf());
    epicsEventSignal(exitEvent);
}

static void callbackShutdown(void *arg)
{
    int i;

    for (i = 0; i < NUM_CALLBACK_PRIORITIES; i++) {
        int lockKey = epicsInterruptLock();
        int ok = epicsRingPointerPush(callbackQ[i], &exitValue);
        epicsInterruptUnlock(lockKey);
        epicsEventSignal(callbackSem[i]);
        if (ok) epicsEventWait(exitEvent);
    }
}

static void callbackInitPvt(void *arg)
{
    int i;

    exitEvent = epicsEventMustCreate(epicsEventEmpty);
    timerQueue = epicsTimerQueueAllocate(0,epicsThreadPriorityScanHigh);
    for (i = 0; i < NUM_CALLBACK_PRIORITIES; i++) {
        epicsThreadId tid;

        callbackSem[i] = epicsEventMustCreate(epicsEventEmpty);
        callbackQ[i] = epicsRingPointerCreate(callbackQueueSize);
        if (callbackQ[i] == 0)
            cantProceed("epicsRingPointerCreate failed for %s\n",
                threadName[i]);
        ringOverflow[i] = FALSE;
        tid = epicsThreadCreate(threadName[i], threadPriority[i],
            epicsThreadGetStackSize(epicsThreadStackBig),
            (EPICSTHREADFUNC)callbackTask, &priorityValue[i]);
        if (tid == 0)
            cantProceed("Failed to spawn callback task %s\n", threadName[i]);
    }
    epicsAtExit(callbackShutdown, NULL);
}

void callbackInit(void)
{
    epicsThreadOnce(&callbackOnceFlag,callbackInitPvt,NULL);
}

/* This routine can be called from interrupt context */
void callbackRequest(CALLBACK *pcallback)
{
    int priority = pcallback->priority;
    int pushOK;
    int lockKey;

    if (priority < 0 || priority >= NUM_CALLBACK_PRIORITIES) {
        epicsPrintf("callbackRequest called with invalid priority\n");
        return;
    }
    if (ringOverflow[priority]) return;

    lockKey = epicsInterruptLock();
    pushOK = epicsRingPointerPush(callbackQ[priority], pcallback);
    epicsInterruptUnlock(lockKey);

    if (!pushOK) {
        errlogPrintf("callbackRequest: %s ring buffer full\n",
            threadName[priority]);
        ringOverflow[priority] = TRUE;
    }
    epicsEventSignal(callbackSem[priority]);
}

static void ProcessCallback(CALLBACK *pcallback)
{
    dbCommon *pRec;

    callbackGetUser(pRec, pcallback);
    dbScanLock(pRec);
    (*pRec->rset->process)(pRec);
    dbScanUnlock(pRec);
}

void callbackSetProcess(CALLBACK *pcallback, int Priority, void *pRec)
{
    callbackSetCallback(ProcessCallback, pcallback);
    callbackSetPriority(Priority, pcallback);
    callbackSetUser(pRec, pcallback);
}

void callbackRequestProcessCallback(CALLBACK *pcallback,
    int Priority, void *pRec)
{
    callbackSetProcess(pcallback, Priority, pRec);
    callbackRequest(pcallback);
}

static void notify(void *pPrivate)
{
    CALLBACK *pcallback = (CALLBACK *)pPrivate;
    callbackRequest(pcallback);
}

void callbackRequestDelayed(CALLBACK *pcallback, double seconds)
{
    epicsTimerId timer = (epicsTimerId)pcallback->timer;

    if (timer == 0) {
        timer = epicsTimerQueueCreateTimer(timerQueue, notify, pcallback);
        pcallback->timer = timer;
    }
    epicsTimerStartDelay(timer, seconds);
}

void callbackCancelDelayed(CALLBACK *pcallback)
{
    epicsTimerId timer = (epicsTimerId)pcallback->timer;

    if (timer != 0) {
        epicsTimerCancel(timer);
    }
}

void callbackRequestProcessCallbackDelayed(CALLBACK *pcallback,
    int Priority, void *pRec, double seconds)
{
    callbackSetProcess(pcallback, Priority, pRec);
    callbackRequestDelayed(pcallback, seconds);
}
