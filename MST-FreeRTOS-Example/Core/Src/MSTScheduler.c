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
	//we have that f_step = 1Mhz and Tstep = 1us, this way we can count us
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static uint32_t prvMSTGetUS() {
	uint32_t outUS = (DWT->CYCCNT / (24));
	return outUS;
}

#endif

typedef enum taskType_e {
	taskTypePeriodic, taskTypeSporadic
} taskType_e;

static void prvMSTPeriodicGenericJob(void*);
static void prvMSTSporadicGenericJob(void*);
static void prvMSTDispatch(TaskHandle_t*, BaseType_t, taskType_e, BaseType_t);

#if(mst_test_PERIODIC_METHOD == 2)
static void prvMSTPeriodicTimerCallback(TimerHandle_t);
#endif

static void prvMSTSporadicTimerCallback(TimerHandle_t);

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

static TaskHandle_t SporadicServerHandle;
#if (mst_USE_SPORADIC_SERVER == 1 && mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS)
static void prvMSTSporadicServerJob(void *pvParameters);
#endif

/*-----------------------------------------------------------*/

/*
 * Task control block.  A task control block (TCB) is allocated for each task,
 * and stores task state information, including a pointer to the task's context
 * (the task's run time environment, including register values)
 * MST4FreeRTOS extends the original TCB, which is still present.
 * The extended TCB is kept in local thread memory for each thread
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

/*
 For periodic server
 TODO: Might be better to put own tcb for sporadic server
 TickType_t xBaseBudget;
 TickType_t xCurrentBudget;
 */

} extTCB_t;

#if(mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF)
static BaseType_t prvAsmissionControlEDF(extTCB_t *);
float prvPeriodicTasksDensity = 0;
#endif

static BaseType_t prvPeriodicTaskCreate(extTCB_t *xFromTCB) {

	if (xFromTCB->pxCreatedTask == &SporadicServerHandle) {
		/*
		 The periodic task passed is the sporadic server, it shall not
		 pass the prvMSTPeriodicGenericJob but the prvMSTSporadicServerJob
		 */
		if (xTaskCreate(prvMSTSporadicServerJob, xFromTCB->pcName,
				xFromTCB->usStackDepth, xFromTCB->pvParameters,
				xFromTCB->uxPriority, xFromTCB->pxCreatedTask) == pdPASS) {
			/*
			 No need for timer since called by aperiodic/sporadic task
			 */
			vTaskSetThreadLocalStoragePointer(*(xFromTCB->pxCreatedTask),
			mstLOCAL_STORAGE_DATA_INDEX, xFromTCB);
			return pdPASS;
		} else {
			return pdFAIL;
		}
	} else {
		/*
		 Generic periodic task
		 */
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
}

/*
 Adds a certain item to the generic xTaskList
 */
static BaseType_t prvAddItemToxTasksList(extTCB_t *fromTCB,
		BaseType_t withValue) {
	if (vTasksListInit == pdFALSE) {
		vTasksListInit = pdTRUE;
		vListInitialise(&xTasksList);
		xListTasksNumber = 0;
	}
	if (!listIS_CONTAINED_WITHIN(&xTasksList, &fromTCB->pxTaskTCBListItem)) {
		vListInitialiseItem(&(fromTCB->pxTaskTCBListItem));
		listSET_LIST_ITEM_OWNER(&(fromTCB->pxTaskTCBListItem), fromTCB);
		listSET_LIST_ITEM_VALUE(&(fromTCB->pxTaskTCBListItem), withValue);
		vListInsertEnd(&xTasksList, &(fromTCB->pxTaskTCBListItem));
		xListTasksNumber++;
		return pdTRUE;
	}
	return pdFALSE;
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
	/*
	 If RMS we fill a list containg all the declared tasks by the user before kernel init.
	 They will be reordered and properly set-up as the kernel start is called
	 */

	if (pxCreatedTask == &SporadicServerHandle) {
		/*
		 The periodic task passed is the sporadic server, it shall not
		 pass the prvMSTPeriodicGenericJob but the prvMSTSporadicServerJob
		 */
		prvPeriodicTaskCreate(xNewExtTCB);
	} else {
		prvAddItemToxTasksList(xNewExtTCB, xTaskPeriod);
	}

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
 * It uses 2 possible methods to handle periodic jobs:
 * mst_test_PERIODIC_METHOD 1: Calls a simple delay
 * mst_test_PERIODIC_METHOD 2: Uses FreeRTOS global timers to callback the task appropriately.
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

#if mst_USE_SPORADIC_SERVER == 0
static void prvMSTSporadicTimerCallback(TimerHandle_t xTimer) {
	/*
	 Notify a sporadic task but make sure it knows it was the timer to notify, by passing a parameter
	 */
	TaskHandle_t *param = (TaskHandle_t*) pvTimerGetTimerID(xTimer);
	prvMSTDispatch(param, true, taskTypeSporadic, false);
}

#endif

#endif   /* #if(mst_test_PERIODIC_METHOD == 2) */

static TickType_t prvXMaxInterrarrivalTime = 0;
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
		//we should give it the priority of the ss if ss activated
		//if (pxCreatedTask == &SporadicServerHandle) uxPriority
		*xNewExtTCB = (extTCB_t ) { .pvJobCode = pvJobCode, .pcName = pcName,
						.pvParameters = pvParameters, .uxPriority = uxPriority,
						.pxCreatedTask = pxCreatedTask, .xTaskDeadline =
								xTaskDeadline, .taskType = taskTypeSporadic,
						.uNumOfMissedDeadlines = 0, .xTaskInterarrivalTime =
								xTaskInterarrivalTime, .xJobCalled = pdFALSE,
						.xInterarrivalTimerRunning =
						pdFALSE, .xTaskWCET =  xTaskWCET};
		prvXMaxInterrarrivalTime =
				(xTaskInterarrivalTime > prvXMaxInterrarrivalTime) ?
						xTaskInterarrivalTime : prvXMaxInterrarrivalTime;
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
 * @brief This represents the generic sporadic job handler.
 * This thread waits for notifications from either
 * - User direct call of sporadic task
 * - Interarrival timer unlocks previously called job
 *
 * @param pvParameters
 */

static BaseType_t getTaskRunTime(TaskHandle_t handle) {
	TaskStatus_t status;
	UBaseType_t count = uxTaskGetSystemState(&status, 1, NULL);
	if (count == 1 && status.xHandle == handle) {
		return status.ulRunTimeCounter;
	}
	return 0;
}
static void prvMSTSporadicGenericJob(void *pvParameters) {

	TaskHandle_t xCurrentHandle = xTaskGetCurrentTaskHandle();
	extTCB_t *xCurrExtTCB = (extTCB_t*) pvTaskGetThreadLocalStoragePointer(
			xCurrentHandle, mstLOCAL_STORAGE_DATA_INDEX);
	configASSERT(xCurrExtTCB != NULL);
	for (;;) {

#if mst_USE_SPORADIC_SERVER == 0
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

#else
		/*
		 Using a sporadic server, no handling of interarrival, only holding
		 a lock on SS request
		 */
		xCurrExtTCB->xJobCalled = pdFALSE;
		uint32_t notificationGiver;
		if (xTaskNotifyWait(0, NOTIFY_SS_REQUEST, &notificationGiver,
		portMAX_DELAY) == pdPASS) {
			xCurrExtTCB->xJobCalled = pdTRUE;
		}
#endif

		BaseType_t actualCPUCycles = getTaskRunTime(xCurrentHandle);
		xCurrExtTCB->xPrevStartTime = xTaskGetTickCount();
		xCurrExtTCB->pvJobCode(pvParameters);
		/*
		 If periodic job is called within the task itself its no problem since the timer is going
		 */
		xCurrExtTCB->xPrevFinishTime = xTaskGetTickCount();
		/*
		 The user shall have set up runtime stats appropriately:
		 1 tick->1us, hence 'actualCPUCycles' is in us
		 */
		//actualCPUCycles = getTaskRunTime(xCurrentHandle) - actualCPUCycles;
		actualCPUCycles = xCurrExtTCB->xPrevFinishTime
				- xCurrExtTCB->xPrevStartTime;

		//xCurrExtTCB->xPrevExecTime = xCurrExtTCB->xPrevFinishTime - xCurrExtTCB->xPrevStartTime;
		xCurrExtTCB->xPrevExecTime = actualCPUCycles;
#if mst_USE_SPORADIC_SERVER == 1
		/*
		 Job over, notify sporadic server
		 */
		xCurrExtTCB->xJobCalled = pdFALSE;
		xTaskNotify(SporadicServerHandle, NOTIFY_SS_REQUEST_DONE, eSetBits);
#endif
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
	 We check if the task has been created. To run the sporadic task we call a dispatch
	 */
	extTCB_t *xCurrExtTCB = (extTCB_t*) pvTaskGetThreadLocalStoragePointer(
			*pxTaskToRunHandle, mstLOCAL_STORAGE_DATA_INDEX);

	if (xCurrExtTCB == NULL) {
		return pdFAIL;
	} else {
		prvMSTDispatch(pxTaskToRunHandle, pdTRUE, taskTypeSporadic, pdTRUE);
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
	return 0;
#elif mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF
	if (itm1->xTaskDeadline < itm2->xTaskDeadline)
		return -1;
	if (itm1->xTaskDeadline > itm2->xTaskDeadline)
		return 1;
	return 0; 
#endif

}

static void prvComputeSporadicServerProprierties(TickType_t *tSS_Period,
		TickType_t *tSS_WCET) {
	/*
	 Rationale:
	 We want to have a sporadic server that has appropriate period and WCET considering the context
	 of other periodic tasks.
	 For Liu-Layland 1973 we have an optimal utilization bound:
	 U = m(2^(1/m) - 1) with 'n' being the number of periodic tasks
	 If we have n periodic tasks + the sporadic server we have m = n + 1 total periodic tasks 
	 (we consider case where sporadic server runs at each period).
	 Considering Css as WCET for the sporadic server and Tss as its period.
	 We then have, considering U = C/T:
	 U = sum(U_p) + C_ss/T_ss = sum(U_p) + U_ss
	 -> U_ss = m(2^(1/m) - 1) - sum(U_p) 
	 With this we get an ideal U_ss, if we find a good value for C_ss (WCET) we can get an ideal period:

	 Lets declare a min and max WCET for the ss:
	 - min: maximum WCET of all periodic tasks
	 - max: sum of all WCET of all periodic tasks

	 At the same time we want the period Tss to be lower or equal to the minimum interarrival
	 time of all sporadics:

	 So we can evaluate:
	 T_ss = C_ss_i/U_ss

	 So here we try every possibility from max C_ss to min C_ss
	 */
	float m = xTasksList.uxNumberOfItems + 1;
	float U = m * (pow(2.0, 1.0 / m) - 1.0);
	TickType_t min_css = 0;
	TickType_t max_css = 0;
	float sum_up = 0; //
	ListItem_t *xItm = listGET_HEAD_ENTRY(&xTasksList); // this is xListEnd.pxNext
	for (int i = 0; i < xListTasksNumber; i++) {
		extTCB_t *xTCB = (extTCB_t*) xItm->pvOwner;
		configASSERT(xTCB != NULL);  // Make sure we don't dereference garbage
		TickType_t t_wcet = xTCB->xTaskWCET;
		TickType_t t_per = xTCB->xTaskPeriod;
		if (t_wcet > min_css) {
			min_css = t_wcet;
		}
		max_css += t_wcet;
		configASSERT(t_per > 0);
		sum_up += ((float) (t_wcet) / (float) (t_per));
		xItm = listGET_NEXT(xItm);
	}
	float Css = max_css;
	float Uss = U - sum_up;
	TickType_t Tss = 0;
	while (Css >= min_css) {
		configASSERT(Uss > 0);
		Tss = Css / Uss;
		if (Tss > prvXMaxInterrarrivalTime) {
			//not ok tss, keep going
			Css -= 1;
		} else {
			//ok tss
			break;
		}
	}
	*tSS_Period = Tss;
	*tSS_WCET = Css;
}

#endif

#if (mst_USE_SPORADIC_SERVER == 1 && mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS)
/*
 * In the MST version of the sporadic server, only one sporadic job runs at a time.
 * To calculate acceptance we simply need to evaluate the slack:
 * σ(t)=⌊(d_s1​−t)/p_s​⌋e_s​−e_s1​
 * Where:
 * - d_s1: Absolute deadline of the sporadic job
 * - p_s: Period of the sporadic server
 * - e_s: Execution budget of the sporadic server (deadline)
 * - e_s1: Execution time requested by the sporadic job S_1
 *
 * The slack of the job is the difference between the guaranteed available
 * processor time and the job’s execution requirement.
 *
 * It shall be higher or equal than 0
 */
static BaseType_t prvAsmissionControlSporadicServer(extTCB_t *forTCB,
		extTCB_t *withSSTCB, TickType_t atArrivalTime) {
	BaseType_t xSlack;
	float guaranteedExecTime =
	    ((float) (forTCB->xTaskDeadline) / (float) withSSTCB->xTaskPeriod)
	    * (float) withSSTCB->xTaskWCET;
	xSlack = guaranteedExecTime - forTCB->xTaskWCET;
	if (xSlack >= 0)
		return pdPASS;
	return pdFAIL;
}

#endif

/*
 This is the sporadic server Job, has the responsibility of:
 - Run acceptance on sporadic jobs
 - Run sporadic jobs
 - Keep track of budget
 - Replenish

 It runs when called from sporadic jobs.
 When a sporadic job finishes the dispatcher notifies the server. If no running jobs the server stops
 and calculates the remaining budget. 

 After every sporadic run the sporadic server calculates the remaining budget, and plans a dedicated
 timer for replenishment.

 Replenishment rule: 
 If a task with period Tp uses budget 'n', a replenishment timer is set to start at Tp + n, and replenishes 'n' units.

 Another timer is always active, it is a watchdog timer, its always updated and
 gets called when the budget is finished. When this happens the running task is suspended until
 next replenishment.

 Only 1 task at a time is ran.

 There is 1 data structure, kept at EDF order. The first in the list is the first to run.
 */

#if (mst_USE_SPORADIC_SERVER == 1 && mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS)

volatile BaseType_t SporadicServerBudget = 0;

volatile BaseType_t isSSJobSuspended = pdFALSE;

static void prvMSTBudgetWatchdogCallback(TimerHandle_t xTimer) {
	/*
	 This is a naive timer, not actually correct as it does not
	 consider that tasks can be preempted while running
	 */
	extTCB_t *xTCB = (extTCB_t*) pvTimerGetTimerID(xTimer);

	taskENTER_CRITICAL();
	SporadicServerBudget = 0;  // budget fully consumed

	// Suspend the job if still running
	if (xTCB->xJobCalled) {
		vTaskSuspend(*(xTCB->pxCreatedTask));
		isSSJobSuspended = pdTRUE;

		// Notify the server task that budget was exhausted
		xTaskNotify(SporadicServerHandle, NOTIFY_SS_BUDGET_FINISHED, eSetBits);
		taskEXIT_CRITICAL();
	}
}

static void prvMSTGenericReplenishmentTimerCallback(TimerHandle_t xTimer) {
	/*
	 Notify a sporadic task but make sure it knows it was the timer to notify, by passing a parameter
	 */
	taskENTER_CRITICAL();
	BaseType_t param = (BaseType_t) pvTimerGetTimerID(xTimer);
	SporadicServerBudget += param;
	xTaskNotify(SporadicServerHandle, NOTIFY_SS_BUDGET_REPLENISHED, eSetBits);
	taskEXIT_CRITICAL();
}

static void prvRemoveSSItemFromList(extTCB_t *forTCB) {
	taskENTER_CRITICAL();
	if (listIS_CONTAINED_WITHIN(&xTasksList, &(forTCB->pxTaskTCBListItem))) {
		uxListRemove(&(forTCB->pxTaskTCBListItem));
		xListTasksNumber--;
	} else {
		configASSERT(pdFALSE);
	}
	taskEXIT_CRITICAL();
}

volatile BaseType_t isNextTaskAvailable = pdFALSE;

static void prvMSTSporadicServerJob(void *pvParameters) {

	TaskHandle_t xCurrentHandle = xTaskGetCurrentTaskHandle();
	extTCB_t *xCurrExtTCB = (extTCB_t*) pvTaskGetThreadLocalStoragePointer(
			xCurrentHandle, mstLOCAL_STORAGE_DATA_INDEX);
	configASSERT(xCurrExtTCB != NULL);
	SporadicServerBudget = xCurrExtTCB->xTaskWCET;

	TimerHandle_t xReplenishmentTimer = xTimerCreate("ReplenishmentTimer",
			pdMS_TO_TICKS(100), // Temporary period
			pdFALSE,
			NULL,               // Temporary placeholder
			prvMSTGenericReplenishmentTimerCallback);

	TimerHandle_t xBudgetWatchdogTimer = xTimerCreate("BudgetWatchdog",
			pdMS_TO_TICKS(SporadicServerBudget), // period = current budget
			pdFALSE,
			NULL, prvMSTBudgetWatchdogCallback);

	extTCB_t *xCurrentRunningSporadicTCB = NULL;
	for (;;) {
		uint32_t ulNotifyVal = 0;
		while (SporadicServerBudget <= 0 || listLIST_IS_EMPTY(&xTasksList)) {
			xTaskNotifyWait(0,
			NOTIFY_SS_NEW_JOB_AVAILABLE | NOTIFY_SS_BUDGET_REPLENISHED,
					&ulNotifyVal, portMAX_DELAY);
			/*
			 * Act on new job or replenishment. Otherwise sleep
			 */
		}
		if (xCurrentRunningSporadicTCB != NULL
				&& xCurrentRunningSporadicTCB->xJobCalled
				&& isSSJobSuspended == pdFALSE) {
			//task is running
			continue;
		} else {
			/*run the task by notifying a SS request
			 This will put xJobCalled = pdTRUE
			 */
			ListItem_t *xItm = listGET_HEAD_ENTRY(&xTasksList);
			extTCB_t *xTCB = (extTCB_t*) xItm->pvOwner;
			xCurrentRunningSporadicTCB = xTCB;


			if (prvAsmissionControlSporadicServer(
					xCurrentRunningSporadicTCB, xCurrExtTCB, xTaskGetTickCount()) == pdFAIL) {
				prvRemoveSSItemFromList(xCurrentRunningSporadicTCB);
			}
			//notify a run request
			xTaskNotify(*(xCurrentRunningSporadicTCB->pxCreatedTask),
					NOTIFY_SS_REQUEST, eSetBits);
			//start budget watchdog
			vTimerSetTimerID(xBudgetWatchdogTimer,
					(void*) xCurrentRunningSporadicTCB);
			if (SporadicServerBudget > 0) {
				if (isSSJobSuspended == pdTRUE) {
					taskENTER_CRITICAL();
					isSSJobSuspended = pdFALSE;
					taskEXIT_CRITICAL();
					vTaskResume(*(xCurrentRunningSporadicTCB->pxCreatedTask));
				}
				BaseType_t xResult = xTimerChangePeriod(xBudgetWatchdogTimer,
						SporadicServerBudget, 0);
				configASSERT(xResult == pdPASS);
				configASSERT(xBudgetWatchdogTimer != NULL);
				//xTimerStart(xBudgetWatchdogTimer, 0);
			}

			uint32_t notificationGiver;
			if (xTaskNotifyWait(0,
			NOTIFY_SS_REQUEST_DONE | NOTIFY_SS_BUDGET_FINISHED,
					&notificationGiver, portMAX_DELAY) == pdPASS) {
				if (notificationGiver & NOTIFY_SS_REQUEST_DONE) {
					/*the Sporadic job finished, we remove budget and
					 schedule replenishment. Note that xPrevExecTime considers
					 only the time for which the job actually ran in us, time used
					 form preempted tasks by the FreeRTOS kernel are not considered
					 */
					taskENTER_CRITICAL();
					xTimerStop(xBudgetWatchdogTimer, 0);
					SporadicServerBudget = SporadicServerBudget
							- xTCB->xPrevExecTime;
					if (SporadicServerBudget < 0) {
						//This should never happen
						SporadicServerBudget = 0;
					}
					/*
					 Now schedule replenishment in time period + exec. time
					 */

					/*
					 Now remove the job and start the replenishment timer
					 */
					prvRemoveSSItemFromList(xCurrentRunningSporadicTCB);
					if (xCurrentRunningSporadicTCB->xPrevExecTime > 0) {

						TickType_t xNewPeriod =
								pdMS_TO_TICKS(
										xCurrExtTCB->xTaskPeriod
												+ xCurrentRunningSporadicTCB->xPrevExecTime);
						BaseType_t xResult = xTimerChangePeriod(
								xReplenishmentTimer, xNewPeriod, 0);
						vTimerSetTimerID(xReplenishmentTimer,
								(void*) xCurrentRunningSporadicTCB->xPrevExecTime);
						configASSERT(xResult == pdPASS);
						configASSERT(
								xTimerStart(xReplenishmentTimer, 0) == pdPASS);
					}

					taskEXIT_CRITICAL();

				} else if (notificationGiver & NOTIFY_SS_BUDGET_FINISHED) {
					//the Sporadic job did not finish but budget is over
					//This shall never happen, prevented by acceptance test!
				}
				//the job returned
			}
		}
	}
}
#endif

/*
 *TODO: This can be obviously optimized by using an appropriate data structure
 Now it does quicksort after getting list, we have O(n log n) in best case
 */

static BaseType_t prvComputeOrderedPriorities() {
	static BaseType_t initialEvaluation = pdTRUE;
	extTCB_t *listArray[xListTasksNumber];
	//transfer linked list to array
	//configASSERT(xTasksList.uxNumberOfItems == xListTasksNumber);
	ListItem_t *xItm = listGET_HEAD_ENTRY(&xTasksList); // this is xListEnd.pxNext
	for (int i = 0; i < xListTasksNumber; i++) {
		extTCB_t *xTCB = (extTCB_t*) xItm->pvOwner;

		configASSERT(xTCB != NULL);  // Make sure we don't dereference garbage
		listArray[i] = xTCB;
		xItm = listGET_NEXT(xItm);

	}

	qsort(listArray, xListTasksNumber, sizeof(extTCB_t*), prv_compare);
#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS
	if (initialEvaluation == pdTRUE) {
		initialEvaluation = pdFALSE;
	} else {
		/*
		 * If RMS calls this after init it means it was called from
		 * sporadic server to sort EDF style the sporadic tasks
		 * (but without changing the tasks priority)
		 */
		return pdPASS;
	}
#endif
	UBaseType_t uxUsedPriority = prvSTARTING_PRIORITY;

	for (UBaseType_t xCurrInx = 0; xCurrInx < xListTasksNumber; xCurrInx++) {
		extTCB_t *xTCB_Reference = listArray[xCurrInx];
		UBaseType_t bNewPriority = uxUsedPriority++;
		xTCB_Reference->uxPriority = bNewPriority;
#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS
		/*
		 * Here we create the periodic tasks that we saved in the list
		 * For RMS this function is called at startup to set priorities,
		 * so we know these are all periodic tasks.
		 */
		configASSERT(prvPeriodicTaskCreate(xTCB_Reference));

#elif mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF
		/*
		* If EDF we simply set priorities accordingly
		*/
		configASSERT(xTCB_Reference->pxCreatedTask != NULL);
		vTaskPrioritySet(*(xTCB_Reference->pxCreatedTask), bNewPriority);
#endif
	}
#if (mst_USE_SPORADIC_SERVER == 1 && mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS)
	/*
	 The list has been used to set priorities for RMS.
	 It now will be used to track sporadic jobs in EDF style to be handled
	 by the sporadic server, so we empty the list.
	 */
	while (!listLIST_IS_EMPTY(&xTasksList)) {
		ListItem_t *pxItem = listGET_HEAD_ENTRY(&xTasksList); // first real item
		extTCB_t *xTCB = (extTCB_t*) pxItem->pvOwner;
		configASSERT(xTCB != NULL);
		uxListRemove(pxItem);
	}
	xListTasksNumber = 0;
#endif
	return pdPASS;
}

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
	/*
	 We compute the sporadic servers WCET and Tss, how this is done is better explained in the function called
	 */
	TickType_t tSS_Period;
	TickType_t tSS_WCET;
	prvComputeSporadicServerProprierties(&tSS_Period, &tSS_WCET);
	/*
	 The sporadic server task is created like a normal "user-space" periodic task (priority is not relevant as recomputed later for RMS)
	 */
	vMSTPeriodicTaskCreate(prvMSTSporadicServerJob, "SS",
	mst_SPORADIC_SERVER_STACK_SIZE, NULL, pdFALSE, &SporadicServerHandle,
			tSS_Period, tSS_Period, 0, tSS_WCET);
#endif
	configASSERT(prvComputeOrderedPriorities());

#else
	//Not RMS
	configASSERT(mst_USE_SPORADIC_SERVER == 0);
#endif

	vTaskStartScheduler();

}

/**
 * @brief This evaluates if a sporadic job can be admitted in dynamic scheduling. The following rule is applied:
 * For a sporadic job S(t,d,e) with:
 * • t: current time
 * • e: execution time (WCET)
 * • d: deadline
 * Let Il be the interval containing the deadline d of the new sporadic job S(t,d,e)
 * Accept if e/(d-t) + delta_s_k <= 1 - delta , for all k=1, 2, ..., l
 * where:
 * • delta_s_k: density of all already active jobs in interval Ik
 * • delta: density of periodic tasks, calculated at kernel start
 */

#if(mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF)

//This can be optimized. The first two iterations can be put into one by 
//calculating past densities at each step
static BaseType_t prvAsmissionControlEDF(extTCB_t *forTCB){
	/*
	The EDF scheduler maintains a list of the jobs, in non-decreasing order of deadline (xTasksList)
	We need to check te intervals up to the new jobs deadline and calculate the density

	We start by calculating all intervals Ik and storing them
	*/
	 
	TickType_t xPrev = 0;
	TickType_t intervalsArray[xListTasksNumber+1];
	TickType_t intervalsStarts[xListTasksNumber+1];
	BaseType_t numOfIntervals;
	TickType_t now = xTaskGetTickCount();
	TickType_t xNewJobAbsDeadline = now + forTCB->xTaskDeadline;
	TickType_t xTaskAbsDeadline = 0;

		ListItem_t *xItm = listGET_HEAD_ENTRY(&xTasksList); // this is xListEnd.pxNext
		for (int i = 0; i < xListTasksNumber; i++) {
			extTCB_t *xTCB = (extTCB_t *) xItm->pvOwner;
			xTaskAbsDeadline = xTCB->xPrevStartTime + xTCB->xTaskDeadline;
			TickType_t xDval = xTaskAbsDeadline - now;
			if(xDval < xNewJobAbsDeadline){
				//calculate d-t for certain delta
				intervalsArray[i] = xDval - xPrev;
				intervalsStarts[i] = xPrev;
				xPrev = xDval;
				numOfIntervals++;
			}else{
				//calculate last interval density with new job deadline
				intervalsArray[i] = xNewJobAbsDeadline - xPrev;
				intervalsStarts[i] = xPrev;
				numOfIntervals++;
				break;
			}
			configASSERT(xTCB != NULL);  
			
			xItm = listGET_NEXT(xItm);

		}

	/*
	Now we calculate the density of each interval (fixed interval, all current tasks)
	When iterating, the task shall add to the density only if its interval overlaps, hence,
	only if its absolute deadline is higher than the interval start
	*/

	float densitiesArray[numOfIntervals];
	memset(densitiesArray, 0, sizeof(densitiesArray));

	xItm = listGET_HEAD_ENTRY(&xTasksList); // this is xListEnd.pxNext
		for (int i = 0; i < xListTasksNumber; i++) {
			extTCB_t *xTCB = (extTCB_t *) xItm->pvOwner;
			xTaskAbsDeadline = xTCB->xPrevStartTime + xTCB->xTaskDeadline;
			for(int j = 0; j < numOfIntervals; j++){
				if(xTaskAbsDeadline >= intervalsStarts[j]){
					//to consider for the density
					densitiesArray[j] += ((float)(xTCB->xTaskWCET))/intervalsArray[j];
				}else{
					//not to be considered also for next intervals
					break;
				}
			}
			configASSERT(xTCB != NULL);  
			
			xItm = listGET_NEXT(xItm);

		}

	/*
	At this point all densities have been computed, we can run the acceptances
	*/
	for(int i = 0; i <numOfIntervals; i++ ){
		if (((float)forTCB->xTaskWCET / (xNewJobAbsDeadline - now)) 
		+ densitiesArray[i] > 1.0f - prvPeriodicTasksDensity){
				//not accepted
				return pdFALSE;
			}
	}
	return pdTRUE;
}
#endif

/**
 * @brief The dispatch is used to manage the execution of all possible tasks
 * taking into consideration the type of both the task and the global scheduling chosen
 * 
 * @param forTask the handle of the task to run
 * @param xAsCallee if true it is called from a request to run. If false it is from a task that finished running
 * @param xOfTaskType the task can be periodic or sporadic
 * @param xFromUserRequest used for sporadic tasks. If true the user called the task to run. If false the interarrival timer asked to run the task
 */

static void prvMSTDispatch(TaskHandle_t *forTask, BaseType_t xAsCallee,
		taskType_e xOfTaskType, BaseType_t xFromUserRequest) {

	if (xAsCallee == pdTRUE) {
		/*
		 Direct call to run a job
		 */
		extTCB_t *xCurrExtTCB = (extTCB_t*) pvTaskGetThreadLocalStoragePointer(
				*forTask, mstLOCAL_STORAGE_DATA_INDEX);
#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_EDF

		/*
		We run admission control for sporadics
		*/
		if(xOfTaskType == taskTypeSporadic){
			if(!prvAsmissionControlEDF(xCurrExtTCB)){
				//There should be a way to notify user of rejection here!
				return;
			}
		}
		/*
		A job is accepted, we add it to list and recompute priorities
		*/
		taskENTER_CRITICAL();
		{
			prvAddItemToxTasksList(xCurrExtTCB, (xCurrExtTCB->xPrevAbsDeadline + xCurrExtTCB->xTaskDeadline));
		    configASSERT(prvComputeOrderedPriorities());
		}
		taskEXIT_CRITICAL();
#else
		//nothing
#endif
		switch (xOfTaskType) {
		case taskTypePeriodic:
			//run the periodic task by unlocking the semaphore
			xTaskNotifyGive(*forTask);
			break;
		case taskTypeSporadic:
#if (mst_USE_SPORADIC_SERVER == 1)
			/*with sporadic server we update the runnable task thread,
			 the sporadic server task will handle it
			 */
			prvAddItemToxTasksList(xCurrExtTCB, xCurrExtTCB->xTaskDeadline);
			configASSERT(prvComputeOrderedPriorities())
			;
			xTaskNotify(SporadicServerHandle, NOTIFY_SS_NEW_JOB_AVAILABLE,
					eSetBits);
#else
			if (xFromUserRequest == pdTRUE) {
				xTaskNotify(*forTask, NOTIFY_USER_REQUEST, eSetBits);
			} else {
				xTaskNotify(*forTask, NOTIFY_INTERARRIVAL_TIMER, eSetBits);
			}
			#endif
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

