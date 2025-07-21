/*
 Enrico Alberti | 2024 | Politecnico di Milano
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

static void prvMSTPeriodicGenericJob(void *pvParameters);
static void prvMSTSporadicGenericJob(void *pvParameters);

#if(mst_test_PERIODIC_METHOD == 2)
static void prvMSTPeriodicTimerCallback(TimerHandle_t xTimer);
#endif

static void prvMSTSporadicTimerCallback(TimerHandle_t xTimer);

#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS
	static BaseType_t vTasksListInit = pdFALSE;
	static List_t xTasksList;	
	static BaseType_t xPeriodicTasksNumber;
	#define prvSTARTING_PRIORITY 5
#endif

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

enum taskType_e {
	taskTypePeriodic, taskTypeSporadic
};

enum sporadicTaskNotifyGiver_e {
	interarrivalTimer = 1, userRequest
};

/*
 * Task control block.  A task control block (TCB) is allocated for each task,
 * and stores task state information, including a pointer to the task's context
 * (the task's run time environment, including register values)
 */
typedef struct MSTextendedTCB {
	//standard from FreeRTOS scheduler
	TaskFunction_t pvJobCode;
	const char *pcName;
	void *pvParameters;
	UBaseType_t uxPriority;
	TaskHandle_t *pxCreatedTask;
	enum taskType_e taskType;
	//periodic-task-specific
	TickType_t xTaskPeriod;
	TickType_t xTaskDeadline;
	TickType_t xTaskPhase;
	TickType_t xTaskWCET;
	//job specific:
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
	if (xTaskCreate(prvMSTPeriodicGenericJob, pcName, usStackDepth,
			pvParameters, uxPriority, pxCreatedTask) == pdPASS) {
		/*
		 Allocate, fill extended TCB using local task storage
		 */

		xNewExtTCB = (extTCB_t*) pvPortMalloc(sizeof(extTCB_t));
		*xNewExtTCB = (extTCB_t ) { .pvJobCode = pvJobCode, .pcName = pcName,
						.pvParameters = pvParameters, .uxPriority = uxPriority,
						.pxCreatedTask = pxCreatedTask, .xTaskPeriod =
								xTaskPeriod, .xTaskDeadline = xTaskDeadline,
						.xTaskPhase = xTaskPhase, .xTaskWCET = xTaskWCET,
						.xPrevReleaseTime = 0, .xPrevExecTime = 0,
						.xPrevAbsDeadline = 0, .xPrevStartTime = 0,
						.xPrevFinishTime = 0, .xNextIdealReleaseTime = 0,
						.xTaskInitDone = pdFALSE, .uNumOfMissedDeadlines = 0 };

		#if (mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS)
			if(vTasksListInit == pdFALSE){
				vTasksListInit = pdTRUE;
				vListInitialise(&xTasksList);
				xPeriodicTasksNumber = 0;
			}
			ListItem_t pxTaskTCBListItem; //SHOULD ADD LIST ITEM TO TCB
			listSET_LIST_ITEM_OWNER(&(pxTaskTCBListItem), xNewExtTCB);
			listSET_LIST_ITEM_VALUE(&(pxTaskTCBListItem), xTaskPeriod);
			vListInitialiseItem(&pxTaskTCBListItem);
			vListInsertEnd(&xTasksList, &pxTaskTCBListItem);
			xPeriodicTasksNumber++;
		#endif 
        #if (mst_test_PERIODIC_METHOD == 2)
		    /*
		     Create the timer,
		     Handle the specific semaphore for the callback
		     */
		    TimerHandle_t xTimer = xTimerCreate("generic periodic timer", // Name of the timer
		    		pdMS_TO_TICKS(xTaskPhase + xTaskPeriod), // Timer period in ticks
		    		pdTRUE,                                // Auto-reload (periodic)
		    		(void*) pxCreatedTask,               // Task handle as parameter
		    		prvMSTPeriodicTimerCallback                 // Callback function
		    		);
		    xNewExtTCB->xTaskSpecificTimer = xTimer;
		    configASSERT(xTimerStart(xTimer, 0) == pdPASS)
        #endif
		vTaskSetThreadLocalStoragePointer(pvParameters, mstLOCAL_STORAGE_DATA_INDEX, xNewExtTCB);
		return pdPASS;
	} else {
		//task created unsuccesfully
		return pdFAIL;
	}

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
					xCurrExtTCB->xUsAverageReleaseGap = (xCurrExtTCB->xUsFromIdealRelease) / (xCurrExtTCB->xNumOfIterations);
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
		xCurrExtTCB->xPrevExecTime = xCurrExtTCB->xPrevFinishTime - xCurrExtTCB->xPrevStartTime;

		if (xCurrExtTCB->xPrevExecTime > xCurrExtTCB->xTaskDeadline) {
			//current task got over the deadline, make notice of the event
			xCurrExtTCB->uNumOfMissedDeadlines++;
		}
		/*
		 Calculate next ideal release time
		 */
		#if(mst_test_PERIODIC_METHOD == 1)
			if((xCurrExtTCB->xPrevReleaseTime + xCurrExtTCB->xTaskPeriod) < xTaskGetTickCount()){
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
	 Here the callback of a ceratin task timer is called, it shall have the semaphore
	 as parameter
	 */
	TaskHandle_t *param = (TaskHandle_t*) pvTimerGetTimerID(xTimer);

	xTaskNotifyGive(*param);
}

static void prvMSTSporadicTimerCallback(TimerHandle_t xTimer) {
	/*
	 Notify a sporadic task but make sure it knows it was the timer to notify, by passing a parameter
	 */
	TaskHandle_t *param = (TaskHandle_t*) pvTimerGetTimerID(xTimer);
	enum sporadicTaskNotifyGiver_e giver = interarrivalTimer;
	xTaskNotify(*param, (uint32_t )giver, eSetValueWithOverwrite);
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
		vTaskSetThreadLocalStoragePointer(pvParameters, mstLOCAL_STORAGE_DATA_INDEX, xNewExtTCB);
		return pdPASS;
	} else {
		//task created unsuccesfully
		return pdFAIL;
	}
}

/**
 * @brief This represents a generic periodic task:
 * Every task has its own data in the extendedTCB to manage timings
 * This version uses delays
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
		if (xTaskNotifyWait(0, 0, &notificationGiver, portMAX_DELAY) == pdPASS) {
			if (notificationGiver == interarrivalTimer) {
				//it was the interarrival timer that woke up the task
				xCurrExtTCB->xInterarrivalTimerRunning = pdFALSE;
				if (xCurrExtTCB->xJobCalled == pdFALSE) {
					continue;
				}
			} else if (notificationGiver == userRequest) {
				//user requested to run
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
		enum sporadicTaskNotifyGiver_e giver = userRequest;
		xTaskNotify(*pxTaskToRunHandle, (uint32_t )giver, eSetValueWithOverwrite);
		return pdPASS;
	}
}


#if mst_schedSCHEDULING_POLICY == mst_schedSCHEDULING_RMS
	static int compare(const void *arg1, const void *arg2){
		return (((extTCB_t *)((ListItem_t *)arg1)->pvOwner)->xTaskPeriod > ((extTCB_t *)((ListItem_t *)arg2)->pvOwner)->xTaskPeriod);
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
		ListItem_t * listArray[xPeriodicTasksNumber];
		//transfer linked list to array
		int i = 0;
		for(ListItem_t * xItm =  (&xTasksList)->pxIndex; xItm != NULL; xItm = xItm->pxNext, i++){
			listArray[i] = xItm;
			TickType_t deb1 =  (extTCB_t *)listGET_LIST_ITEM_VALUE(listArray[i]);
			//;((extTCB_t *)listArray[i]->pvOwner)->xTaskPeriod;
			int haga = 0;
		}
		//sort array of tasks
		qsort(listArray, xPeriodicTasksNumber, sizeof(ListItem_t), compare);
		//go trough sorted array and sort from priority prvSTARTING_PRIORITY
		static UBaseType_t uxUsedPriority = prvSTARTING_PRIORITY;
		for(UBaseType_t xCurrInx = 0; xCurrInx < xPeriodicTasksNumber; xCurrInx++) (((extTCB_t *)listArray[xCurrInx]->pvOwner)->uxPriority = uxUsedPriority++);
	#endif
	vTaskStartScheduler();
	
}
