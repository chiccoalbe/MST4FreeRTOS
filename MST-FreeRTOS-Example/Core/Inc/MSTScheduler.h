#include "FreeRTOS.h"
#include "task.h"
#include "FreeRTOS.h"
#include "list.h"
#include "timers.h"
#include "task.h"
/*
 Testing macro needed to select the wanted technique for
 periodic task management:
 1- Using FreeRTOS delays
 2- Using software timers
 */
#define mst_test_PERIODIC_METHOD 2

/*
 Default scheduling policy from FreeRTOS
 */
#define mst_schedSCHEDULING_DEFAULT 1

/*
 RMS (rate-monotonic) scheduling policy managed by MST scheduler
 */
#define mst_schedSCHEDULING_RMS 2

/*
 EDF (earliest-deadline-first) scheduling policy managed by MST scheduler
 */
#define mst_schedSCHEDULING_EDF 3

#define mst_schedSCHEDULING_POLICY mst_schedSCHEDULING_DEFAULT

#ifndef configNUM_THREAD_LOCAL_STORAGE_POINTERS
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 3
#endif

#define TESTING_STM32 1

#if(TESTING_STM32 == 1)
/*
 * Timer reference given from user and used by MST to count nanoseconds
 */
#include "stm32f4xx_hal.h" //shall be modified for specific board

#endif

void vMSTSchedulerInit(void);

void vMSTSchedulerStart(void);

/**
 * @brief Creation of a periodic task. 
 * This takes from the xTaskCreate but adds standard 
 * model parameters to handle periodic tasks
 * @param pvJobCode pointer to the function that implements the job
 * @param pcName A descriptive name for the job
 * @param usStackDepth Length of the stack for this task, expressed in words
 * @param pvParameters parameters passed into the job
 * @param uxPriority priority of the job, not used if MST specific scheduling is used
 * @param pxCreatedTask job handle pointer
 * @param xTaskPeriod T_i | period of the task in ms
 * @param xTaskDeadline D_i | relative deadline of the task in ms
 * @param xTaskPhase phi_i | phase of the task in ms (shift from first release)
 * @param xTaskWCET worst case execution time of the task
 * @return 
 */
TaskHandle_t vMSTPeriodicTaskCreate(
		TaskFunction_t pvJobCode,
		const char *const pcName,
		uint16_t usStackDepth,
		void *pvParameters,
		UBaseType_t uxPriority,
		TaskHandle_t *pxCreatedTask,
		/*
		 library-specific parameters
		 */
		TickType_t xTaskPeriod,
		TickType_t xTaskDeadline,
		TickType_t xTaskPhase,
		TickType_t xTaskWCET
		);

/**
 * @brief Creation of a sporadic task. 
 * @param pvJobCode pointer to the function that implements the job
 * @param pcName A descriptive name for the job
 * @param usStackDepth Length of the stack for this task, expressed in words
 * @param pvParameters parameters passed into the job
 * @param uxPriority priority of the job, not used if MST specific scheduling is used
 * @param pxCreatedTask job handle pointer
 * @param xTaskInterarrivalTime T_i | minimum spacing between releases in ms
 * @param xTaskDeadline D_i | relative deadline of the task in ms
 * @param xTaskPhase phi_i | phase of the task in ms (shift from first release)
 * @param xTaskWCET worst case execution time of the task
 * @param xJobCalled the job has been called at exec
 * @return 
 */
BaseType_t vMSTSporadicTaskCreate(
		TaskFunction_t pvJobCode,
		const char *const pcName,
		uint16_t usStackDepth,
		void *pvParameters,
		UBaseType_t uxPriority,
		TaskHandle_t *pxCreatedTask,
		/*
		 library-specific parameters
		 */
		TickType_t xTaskInterarrivalTime,
		TickType_t xTaskDeadline,
		TickType_t xTaskWCET);

/**
 * @brief Runs the sporadic job, if interarrival time has passed
 * 
 * @param pxTaskToRunHandle 
 * @return pdFAIL if the task was never created
 * @return pdPASS if succesfull 
 */
BaseType_t vMSTSporadicTaskRun(TaskHandle_t *pxTaskToRunHandle);
