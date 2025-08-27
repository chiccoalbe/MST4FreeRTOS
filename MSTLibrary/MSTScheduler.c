/*
 Enrico Alberti | 2024-2025 | Politecnico di Milano
 Implementation of the Standard Model FreeRTOS scheduler
 */

#include "MSTScheduler.h"
#include <stdlib.h>
#include <search.h>

#if(TESTING_STM32 == 1)
/*
 * Timer reference given from user and used by MST to count nanoseconds
 */
static float prvMSTSetupUSClock() {
	//set prescaler equal to MHz of clock
	/*__HAL_RCC_TIM2_CLK_ENABLE();
	 tstMSTTimerReferenceFromUser->PSC = (HAL_RCC_GetPCLK1Freq() / 1000000 - 1);
	 RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
	 tstMSTTimerReferenceFromUser->ARR = 0xFFFFFFFF;
	 tstMSTTimerReferenceFromUser->CR1 |= TIM_CR1_CEN;*/
	//we have that f_step = 1Mhz and Tstep = 1us, this way we can count us
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static uint32_t prvMSTGetUS() {
	/*uint32_t out = tstMSTTimerReferenceFromUser->CNT;
	 BaseType_t clckFreq = HAL_RCC_GetPCLK1Freq();
	 BaseType_t countFreq = clckFreq / (tstMSTTimerReferenceFromUser->PSC + 1);
	 float timing = 1.0 / (float) countFreq;*/
	uint32_t outUS = (DWT->CYCCNT / (24));
	return outUS;
}

#endif

typedef enum taskType_e {
	taskTypePeriodic, taskTypeSporadic
}taskType_e;


static void prvMSTPeriodicGenericJob(void *pvParameters);
static void prvMSTSporadicGenericJob(void *pvParameters);
static void prvMSTDispatch(TaskHandle_t *forTask, BaseType_t xAsCallee,
		taskType_e xOfTaskType, BaseType_t xFromUserRequest);

#if(mst_test_PERIODIC_METHOD == 2)
static void prvMSTPeriodicTimerCallback(TimerHandle_t xTimer);
#endif

static void prvMSTSporadicTimerCallback(TimerHandle_t xTimer);

#if (mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS || mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF) 
static BaseType_t vTasksListInit = pdFALSE;
static List_t xTasksList;
static BaseType_t xListTasksNumber;
#define prvSTARTING_PRIORITY 5
#endif

/*
 #if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF
 static BaseType_t vRunnableTasksListInit = pdFALSE;
 static List_t vRunnableTasksList;
 static BaseType_t xActiveTasksNumber;
 #endif*/

/*
 based on configNUM_THREAD_LOCAL_STORAGE_POINTERS > 0
 */

#if(configNUM_THREAD_LOCAL_STORAGE_POINTERS == 0)
//overriding the definition of storage pointer, THIS MIGHT NOT WORK, TO CHECK!
void vTaskSetThreadLocalStoragePointer(TaskHandle_t xTaskToSet,
                                       BaseType_t xIndex, void *pvValue) PRIVILEGED_FUNCTION;
void* pvTaskGetThreadLocalStoragePointer(TaskHandle_t xTaskToQuery,
        BaseType_t xIndex) PRIVILEGED_FUNCTION;
#endif

/*
 Index for data pointer for specific extendedTCB
 */
#define mstLOCAL_STORAGE_DATA_INDEX 0

/*-----------------------------------------------------------*/


/*
 * Task control block.  A task control block (TCB) is allocated for each task,
 * and stores task state information, including a pointer to the task's context
 * (the task's run time environment, including register values)
 */
typedef struct MSTextendedTCB {
	//standard from FreeRTOS scheduler
#if (mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS || mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF) 
	ListItem_t pxTaskTCBListItem; //used for RMS / EDF in different ways
#endif
	TaskFunction_t pvJobCode;
	const char *pcName;
	configSTACK_DEPTH_TYPE usStackDepth;
	void *pvParameters;
	UBaseType_t uxPriority;
	TaskHandle_t *pxCreatedTask;
	enum taskType_e taskType;
	//periodic-task-specific
	TickType_t xTaskPeriod;
	TickType_t xTaskDeadline;
	TickType_t xTaskPhase;
	TickType_t xTaskWCET;
	//job specific:vListInsertEnd
	TickType_t xPrevReleaseTime;
	TickType_t xPrevExecTime;
	TickType_t xPrevAbsDeadline;
	TickType_t xPrevStartTime;
	TickType_t xPrevFinishTime;
	UBaseType_t uNumOfMissedDeadlines;
	TickType_t xNextIdealReleaseTime;
	/*
	 Variable to notice if the phase was done
	 */
	BaseType_t xTaskInitDone;
	/*
	 Semaphore related to the specific task
	 */
#if(mst_test_PERIODIC_METHOD == 2)
	TimerHandle_t xTaskSpecificTimer;
#endif
#if(TESTING_STM32)
	/*
	 * Info about performance
	 */
	int32_t xUsFromIdealRelease;
	uint32_t xNumOfIterations;
	int32_t xUsAverageReleaseGap;
#endif
	/*
	 For sporadic tasks
	 */
	TickType_t xTaskInterarrivalTime;
	BaseType_t xInterarrivalTimerRunning;
	BaseType_t xJobCalled;

} extTCB_t;

static BaseType_t prvPeriodicTaskCreate(extTCB_t *xFromTCB) {
	if (xTaskCreate(prvMSTPeriodicGenericJob, xFromTCB->pcName,
			xFromTCB->usStackDepth, xFromTCB->pvParameters,
			xFromTCB->uxPriority, xFromTCB->pxCreatedTask) == pdPASS) {
#if (mst_test_PERIODIC_METHOD == 2)
		/*
		 Create the timer,
		 Handle the specific semaphore for the callback
		 */
		TimerHandle_t xTimer = xTimerCreate("generic periodic timer",
				pdMS_TO_TICKS(xFromTCB->xTaskPhase + xFromTCB->xTaskPeriod),
				pdTRUE, (void*) (xFromTCB->pxCreatedTask),
				prvMSTPeriodicTimerCallback);
		xFromTCB->xTaskSpecificTimer = xTimer;
		configASSERT(xTimerStart(xTimer, 0) == pdPASS)
#endif
		vTaskSetThreadLocalStoragePointer(*(xFromTCB->pxCreatedTask),
				mstLOCAL_STORAGE_DATA_INDEX, xFromTCB);
		return pdPASS;
	} else {
		return pdFAIL;
	}
}

TaskHandle_t vMSTPeriodicTaskCreate(TaskFunction_t pvJobCode,
		const char *pcName, uint16_t usStackDepth, void *pvParameters,
		UBaseType_t uxPriority, TaskHandle_t *pxCreatedTask,
		/* library-specific parameters */
		TickType_t xTaskPeriod, TickType_t xTaskDeadline, TickType_t xTaskPhase,
		TickType_t xTaskWCET) {
	//needs to call vTaskCreate
	extTCB_t *xNewExtTCB;
	/*
	 We manage task using normal delay wrapper
	 We create the specific data structure to hold data about the job
	 */

	/*
	 Allocate, fill extended TCB using local task storage
	 */

	xNewExtTCB = (extTCB_t*) pvPortMalloc(sizeof(extTCB_t));
	*xNewExtTCB = (extTCB_t ) { .pvJobCode = pvJobCode, .pcName = pcName,
					.usStackDepth = usStackDepth, .pvParameters = pvParameters,
					.uxPriority = uxPriority, .pxCreatedTask = pxCreatedTask,
					.xTaskPeriod = xTaskPeriod, .xTaskDeadline = xTaskDeadline,
					.xTaskPhase = xTaskPhase, .xTaskWCET = xTaskWCET,
					.xPrevReleaseTime = 0, .xPrevExecTime = 0,
					.xPrevAbsDeadline = 0, .xPrevStartTime = 0,
					.xPrevFinishTime = 0, .xNextIdealReleaseTime = 0,
					.xTaskInitDone = pdFALSE, .uNumOfMissedDeadlines = 0 };

#if (mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS)
	if (vTasksListInit == pdFALSE) {
		vTasksListInit = pdTRUE;
		vListInitialise(&xTasksList);
		xListTasksNumber = 0;
	}
	listSET_LIST_ITEM_OWNER(&(xNewExtTCB->pxTaskTCBListItem), xNewExtTCB);
	listSET_LIST_ITEM_VALUE(&(xNewExtTCB->pxTaskTCBListItem), xTaskPeriod);
	vListInitialiseItem(&(xNewExtTCB->pxTaskTCBListItem));
	vListInsertEnd(&xTasksList, &(xNewExtTCB->pxTaskTCBListItem));
	xListTasksNumber++;
	return pdPASS;
#else
	/*
	 * If not RMS we can directly create the task, otherwise it is done before kernel start
	 */
	if(prvPeriodicTaskCreate(xNewExtTCB)) {
		return pdPASS;
	} else {
		return pdFAIL;
	}
#endif

}

/**
 * @brief This represents a generic periodic task:
 * Every task has its own data in the extendedTCB to manage timings
 * This version uses delays
 *
 * @param pvParameters
 */

static void prvMSTPeriodicGenericJob(void *pvParameters) {
#if(mst_test_PERIODIC_METHOD == 2)
	/*
	 Takes notification for current task
	 */
	ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#endif
	/*
	 Get values about current job
	 */
	TaskHandle_t xCurrentHandle = xTaskGetCurrentTaskHandle();
	extTCB_t *xCurrExtTCB = (extTCB_t*) pvTaskGetThreadLocalStoragePointer(
			xCurrentHandle, mstLOCAL_STORAGE_DATA_INDEX);
	/*
	 We assert that the TCB has been retreived, else we compromised exec
	 */
	configASSERT(xCurrExtTCB != NULL);
	/*
	 First iteration with phase passed, change the period to not have the phase
	 */
	if (xCurrExtTCB->xTaskInitDone == pdFALSE) {
		xCurrExtTCB->xTaskInitDone = pdTRUE;
#if(TESTING_STM32)
		//this is the us at start
		xCurrExtTCB->xUsFromIdealRelease = 0;
		xCurrExtTCB->xUsAverageReleaseGap = 0;
#endif

		if (xCurrExtTCB->xTaskPhase > 0) {
			/*
			 If we have a phase, we should have just passed it
			 */
#if(mst_test_PERIODIC_METHOD == 1)
			vTaskDelayUntil(&(xCurrExtTCB->xPrevReleaseTime), xCurrExtTCB->xTaskPhase);
#elif(mst_test_PERIODIC_METHOD == 2)
			xTimerChangePeriod(xCurrExtTCB->xTaskSpecificTimer,
					pdMS_TO_TICKS(xCurrExtTCB->xTaskPeriod), portMAX_DELAY);
#endif
		}

	}

	xCurrExtTCB->xPrevReleaseTime = xTaskGetTickCount();

	for (;;) {

#if(TESTING_STM32)
		//count gap from execution if testing
#if(mst_test_PERIODIC_METHOD == 2)
		xCurrExtTCB->xNumOfIterations++;
#endif
		//calculate the absolute perfect release in us
		uint32_t perfRelease = xCurrExtTCB->xTaskPhase * 1000
				+ xCurrExtTCB->xTaskPeriod * 1000
						* (xCurrExtTCB->xNumOfIterations);
		//calculate the gap between perfect and absolute gap in us and update the avg
		xCurrExtTCB->xUsFromIdealRelease += prvMSTGetUS() - perfRelease;
		if (xCurrExtTCB->xNumOfIterations > 0) {
			xCurrExtTCB->xUsAverageReleaseGap =
					(xCurrExtTCB->xUsFromIdealRelease)
							/ (xCurrExtTCB->xNumOfIterations);
		}
#if(mst_test_PERIODIC_METHOD == 1)
		xCurrExtTCB->xNumOfIterations++;
#endif
#endif
		/*
		 Run user code
		 */
		xCurrExtTCB->xPrevStartTime = xTaskGetTickCount();
		xCurrExtTCB->pvJobCode(pvParameters);
		xCurrExtTCB->xPrevFinishTime = xTaskGetTickCount();
		xCurrExtTCB->xPrevExecTime = xCurrExtTCB->xPrevFinishTime
				- xCurrExtTCB->xPrevStartTime;

		if (xCurrExtTCB->xPrevExecTime > xCurrExtTCB->xTaskDeadline) {
			//current task got over the deadline, make notice of the event
			xCurrExtTCB->uNumOfMissedDeadlines++;
		}
		
	#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF
		/*
		Notify dispatcher of finished job
		*/
		prvMSTDispatch(xCurrExtTCB->pxCreatedTask, false, taskTypePeriodic, false);
	#endif
		
	
#if(mst_test_PERIODIC_METHOD == 1)
		if((xCurrExtTCB->xPrevReleaseTime + xCurrExtTCB->xTaskPeriod) < xTaskGetTickCount()) {
			/*
			Current task overshot next ideal release time, not only the deadline
			We have an error, we update release time
			TODO: see best way of handling this
			*/
			xCurrExtTCB->xPrevReleaseTime = xTaskGetTickCount();
		}
		/*
		Update scheduler-referenced release time
		*/
		//vTaskDelay(pdMS_TO_TICKS(xCurrExtTCB->xTaskPeriod));
		vTaskDelayUntil(&(xCurrExtTCB->xPrevReleaseTime), pdMS_TO_TICKS(xCurrExtTCB->xTaskPeriod));
#elif(mst_test_PERIODIC_METHOD == 2)
		ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
#endif

	}
}

#if(mst_test_PERIODIC_METHOD == 2)
/*
 Using periodic timers with mutexes
 */
static void prvMSTPeriodicTimerCallback(TimerHandle_t xTimer) {
	/*
	 Here the callback of a certain task timer is called, it shall have the semaphore
	 as parameter
	 */
	TaskHandle_t *param = (TaskHandle_t*) pvTimerGetTimerID(xTimer);
	prvMSTDispatch(param, true, taskTypePeriodic, false);
}

static void prvMSTSporadicTimerCallback(TimerHandle_t xTimer) {
	/*
	 Notify a sporadic task but make sure it knows it was the timer to notify, by passing a parameter
	 */
	TaskHandle_t *param = (TaskHandle_t*) pvTimerGetTimerID(xTimer);
	prvMSTDispatch(param, true, taskTypeSporadic, false);
}

#endif   /* #if(mst_test_PERIODIC_METHOD == 2) */

BaseType_t vMSTSporadicTaskCreate(TaskFunction_t pvJobCode, const char *pcName,
		uint16_t usStackDepth, void *pvParameters, UBaseType_t uxPriority,
		TaskHandle_t *pxCreatedTask,
		/* library-specific parameters */
		TickType_t xTaskInterarrivalTime, TickType_t xTaskDeadline,
		TickType_t xTaskWCET) {

	extTCB_t *xNewExtTCB;
	if (xTaskCreate(prvMSTSporadicGenericJob, pcName, usStackDepth,
			pvParameters, uxPriority, pxCreatedTask) == pdPASS) {
		/*
		 Allocate, fill extended TCB using local task storage
		 */
		xNewExtTCB = (extTCB_t*) pvPortMalloc(sizeof(extTCB_t));
		*xNewExtTCB = (extTCB_t ) { .pvJobCode = pvJobCode, .pcName = pcName,
						.pvParameters = pvParameters, .uxPriority = uxPriority,
						.pxCreatedTask = pxCreatedTask, .xTaskDeadline =
								xTaskDeadline, .uNumOfMissedDeadlines = 0,
						.xTaskInterarrivalTime = xTaskInterarrivalTime,
						.xJobCalled = pdFALSE, .xInterarrivalTimerRunning =
						pdFALSE };

		/*
		 We create the task and allocate, but we do not clear the mutex nor start the timer
		 */
		vTaskSetThreadLocalStoragePointer(*pxCreatedTask,
				mstLOCAL_STORAGE_DATA_INDEX, xNewExtTCB);
		return pdPASS;
	} else {
		//task created unsuccesfully
		return pdFAIL;
	}
}

/**
 * @brief
 *
 * @param pvParameters
 */

static void prvMSTSporadicGenericJob(void *pvParameters) {

	TaskHandle_t xCurrentHandle = xTaskGetCurrentTaskHandle();
	extTCB_t *xCurrExtTCB = (extTCB_t*) pvTaskGetThreadLocalStoragePointer(
			xCurrentHandle, mstLOCAL_STORAGE_DATA_INDEX);
	configASSERT(xCurrExtTCB != NULL);
	for (;;) {
		/*
		 Takes notification for current task, could be from timer or user
		 */
		uint32_t notificationGiver;
		if (xTaskNotifyWait(0, NOTIFY_INTERARRIVAL_TIMER | NOTIFY_USER_REQUEST,
				&notificationGiver, portMAX_DELAY) == pdPASS) {
			if (notificationGiver & NOTIFY_INTERARRIVAL_TIMER) {
				//it was the interarrival timer that woke up the task
				xCurrExtTCB->xInterarrivalTimerRunning = pdFALSE;
				notificationGiver &= ~NOTIFY_INTERARRIVAL_TIMER;
				if (xCurrExtTCB->xJobCalled == pdFALSE) {
					continue;
				}
			} else if (notificationGiver & NOTIFY_USER_REQUEST) {
				//user requested to run
				notificationGiver &= ~NOTIFY_USER_REQUEST;
				if (xCurrExtTCB->xInterarrivalTimerRunning == pdTRUE) {
					xCurrExtTCB->xJobCalled = pdTRUE;
					continue;
				}
			}
		}
		/*
		 Notify job called and interarrival timer from release
		 */
		xCurrExtTCB->xJobCalled = pdTRUE;
		TimerHandle_t xTimer = xTimerCreate("sporadic interarrival timer", // Name of the timer
				pdMS_TO_TICKS(xCurrExtTCB->xTaskInterarrivalTime), // Timer period in ticks
				pdFALSE,                               // Auto-reload (periodic)
				(void*) (xCurrExtTCB->pxCreatedTask), // Task handle as parameter
				prvMSTSporadicTimerCallback                 // Callback function
				);
		configASSERT(xTimerStart(xTimer, 0) == pdPASS)
		xCurrExtTCB->xTaskSpecificTimer = xTimer;
		taskENTER_CRITICAL(); //maybe not needed
		xCurrExtTCB->xInterarrivalTimerRunning = pdTRUE;
		xCurrExtTCB->xJobCalled = pdFALSE;
		taskEXIT_CRITICAL();

		xCurrExtTCB->xPrevStartTime = xTaskGetTickCount();
		xCurrExtTCB->pvJobCode(pvParameters);
		/*
		 If periodic job is called within the task itself its no problem since the timer is going
		 */
		xCurrExtTCB->xPrevFinishTime = xTaskGetTickCount();
		xCurrExtTCB->xPrevExecTime = xCurrExtTCB->xPrevFinishTime
				- xCurrExtTCB->xPrevStartTime;

		#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF
		/*
		Notify dispatcher of finished job
		*/
		prvMSTDispatch(xCurrExtTCB->pxCreatedTask, false, taskTypeSporadic, false);
	#endif
	}
}

BaseType_t vMSTSporadicTaskRun(TaskHandle_t *pxTaskToRunHandle) {
	/*
	 We check if the task has been created, one way of doing this is by seeing if the TCB is ok
	 maybe there are other better ways
	 */
	extTCB_t *xCurrExtTCB = (extTCB_t*) pvTaskGetThreadLocalStoragePointer(
			*pxTaskToRunHandle, mstLOCAL_STORAGE_DATA_INDEX);

	if (xCurrExtTCB == NULL) {
		return pdFAIL;
	} else {
		prvMSTDispatch(pxTaskToRunHandle, true, taskTypeSporadic, true);
		return pdPASS;
	}
}

#if (mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS || mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF)
static int prv_compare(const void *arg1, const void *arg2) {
	extTCB_t *itm1 = *(extTCB_t**) arg1;
	extTCB_t *itm2 = *(extTCB_t**) arg2;

#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS

	if (itm1->xTaskPeriod > itm2->xTaskPeriod)
		return -1;
	if (itm1->xTaskPeriod < itm2->xTaskPeriod)
		return 1;
	return 0; // Equal
#elif mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF
	if (itm1->xTaskDeadline < itm2->xTaskDeadline)
		return -1;
	if (itm1->xTaskDeadline > itm2->xTaskDeadline)
		return 1;
	return 0; // Equal
#endif


}
/*
 *TODO: This can be obviously optimized by using an appropriate data structure
 Now it does quicksort after getting list, we have O(n log n) in best case
 */

static BaseType_t prvComputeOrderedPriorities() {
	extTCB_t *listArray[xListTasksNumber];
	//transfer linked list to array
	int i = 0;
	//configASSERT(xTasksList.uxNumberOfItems == xListTasksNumber);
	ListItem_t *xItm = listGET_HEAD_ENTRY(&xTasksList); // this is xListEnd.pxNext
	for (int i = 0; i < xListTasksNumber; i++) {
	    extTCB_t *xTCB = (extTCB_t *) xItm->pvOwner;

	    configASSERT(xTCB != NULL);  // Make sure we don't dereference garbage
	    listArray[i] = xTCB;
	    xItm = listGET_NEXT(xItm);

	}

	qsort(listArray, xListTasksNumber, sizeof(extTCB_t*), prv_compare);
	UBaseType_t uxUsedPriority = prvSTARTING_PRIORITY;

	for (UBaseType_t xCurrInx = 0; xCurrInx < xListTasksNumber; xCurrInx++) {
		extTCB_t *xTCB_Reference = listArray[xCurrInx];
		UBaseType_t bNewPriority = uxUsedPriority++;
		xTCB_Reference->uxPriority = bNewPriority;
#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS
		/*
		 * Here we create the periodic tasks that we saved in the list
		 */
		configASSERT(prvPeriodicTaskCreate(xTCB_Reference));
#elif mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF
		configASSERT(xTCB_Reference->pxCreatedTask != NULL);
			vTaskPrioritySet(*(xTCB_Reference->pxCreatedTask), bNewPriority);
			#endif
	}
	return pdPASS;
}
#endif

/*
 MST version of scheduler start
 */
void vMSTSchedulerStart(void) {
#if(TESTING_STM32 == 1)
	prvMSTSetupUSClock();
#endif
	/*
	 Set rate monotonic priorities
	 */
#if (mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS)
	/*
	 We go trough the created periodic tasks and change priority accordingly
	 */
#if mst_USE_SPORADIC_SERVER == 1
#endif
	configASSERT(prvComputeOrderedPriorities());

#endif

	vTaskStartScheduler();

}


static void prvMSTDispatch(TaskHandle_t *forTask, BaseType_t xAsCallee,
		taskType_e xOfTaskType, BaseType_t xFromUserRequest) {

	if (xAsCallee == pdTRUE) {
#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF
		/*
		A job wants to run, we add it to list and recompute priorities
		*/

		extTCB_t *xCurrExtTCB = (extTCB_t*) pvTaskGetThreadLocalStoragePointer(
	                            *forTask, mstLOCAL_STORAGE_DATA_INDEX);

		taskENTER_CRITICAL();
		{
		    if (vTasksListInit == pdFALSE) {
		        vTasksListInit = pdTRUE;
		        vListInitialise(&xTasksList);
		        xListTasksNumber = 0;
		    }
		    //TODO: This might not be ideal, would we want same task in list? This is a rejection
		    if (!listIS_CONTAINED_WITHIN(&xTasksList, &xCurrExtTCB->pxTaskTCBListItem)) {
		        vListInitialiseItem(&xCurrExtTCB->pxTaskTCBListItem);
		        listSET_LIST_ITEM_OWNER(&xCurrExtTCB->pxTaskTCBListItem, xCurrExtTCB);
		        listSET_LIST_ITEM_VALUE(&xCurrExtTCB->pxTaskTCBListItem,
		            xCurrExtTCB->xPrevAbsDeadline + xCurrExtTCB->xTaskDeadline);

		        vListInsertEnd(&xTasksList, &xCurrExtTCB->pxTaskTCBListItem);
		        xListTasksNumber++;
		    }

		    configASSERT(prvComputeOrderedPriorities());
		}
		taskEXIT_CRITICAL();
#else
		//nothing
#endif
		switch (xOfTaskType) {
		case taskTypePeriodic:
			xTaskNotifyGive(*forTask);
			break;
		case taskTypeSporadic:
			if (xFromUserRequest == pdTRUE) {
				xTaskNotify(*forTask, NOTIFY_USER_REQUEST, eSetBits);
			} else {
				xTaskNotify(*forTask, NOTIFY_INTERARRIVAL_TIMER, eSetBits);
			}
			break;
		default:
			configASSERT(pdFALSE)
			break;
		}

	} else {
		/*
		 Received from a finished job, we shall remove the job from the actively running in the list
		 */
#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF
    extTCB_t *xCurrExtTCB = (extTCB_t*) pvTaskGetThreadLocalStoragePointer(
                                *forTask, mstLOCAL_STORAGE_DATA_INDEX);

    configASSERT(xCurrExtTCB != NULL);

    taskENTER_CRITICAL();
    {
        if (listIS_CONTAINED_WITHIN(&xTasksList, &(xCurrExtTCB->pxTaskTCBListItem))) {
            uxListRemove(&(xCurrExtTCB->pxTaskTCBListItem));
            xListTasksNumber--;
        } else {
            configASSERT(pdFALSE);
        }

        configASSERT(prvComputeOrderedPriorities());
    }
    taskEXIT_CRITICAL();

#endif
	}

}

