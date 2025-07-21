/*! @mainpage My Personal Index Page
 *
 * @section intro_sec Introduction
 *
 * This is the introduction.
 *
 * @section install_sec Installation
 *
 * @subsection step1 Step 1: Opening the box
 *
 * etc...
 */

/**
 * @file MSTScheduler.h
 * @author Enrico Alberti
 * @date 2024
 * @brief Header file for the standard model adaptation library of FreeRTOS.
 * 
 * This file contains definitions and function prototypes for the 
 * Model-Specific Task (MST) scheduler library, which extends FreeRTOS
 * to include additional scheduling policies and support for periodic
 * and sporadic tasks.
 */

#ifndef _MST_SCHEDULER_H
#	define _MST_SCHEDULER_H

#include "FreeRTOS.h"
#include "task.h"
#include "list.h"
#include "timers.h"

/**
 * @brief Macro to select the technique for periodic task management.
 * 
 * Options:
 * - 1: Using FreeRTOS delays.
 * - 2: Using software timers.
 */
#define mst_test_PERIODIC_METHOD 2

/**
 * @brief Default scheduling policy from FreeRTOS.
 */
#define mst_schedSCHEDULING_DEFAULT 1

/**
 * @brief Rate-Monotonic Scheduling (RMS) policy managed by MST scheduler.
 */
#define mst_schedSCHEDULING_RMS 2

/**
 * @brief Earliest-Deadline-First (EDF) scheduling policy managed by MST scheduler.
 */
#define mst_schedSCHEDULING_EDF 3

/**
 * @brief Selected scheduling policy. 
 * 
 * Default is RMS scheduling policy.
 */
#define mst_schedSCHEDULING_POLICY mst_schedSCHEDULING_RMS

#ifndef configNUM_THREAD_LOCAL_STORAGE_POINTERS
/**
 * @brief Number of thread-local storage pointers.
 * 
 * Default is set to 3 if not defined elsewhere.
 */
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 3
#endif

/**
 * @brief Macro indicating STM32 testing environment.
 */
#define TESTING_STM32 1

#if (TESTING_STM32 == 1)
/**
 * @brief Includes the HAL library for STM32F4 boards.
 * 
 * Modify this include for specific STM32 board support.
 */
#include "stm32f4xx_hal.h"

#endif

/**
 * @brief Initializes the MST Scheduler.
 * 
 * This function sets up the MST scheduler, preparing it to manage
 * tasks based on the selected scheduling policy.
 */
void vMSTSchedulerInit(void);

/**
 * @brief Starts the MST Scheduler.
 * 
 * This function begins the task scheduling process using the MST framework.
 */
void vMSTSchedulerStart(void);

/**
 * @brief Creates a periodic task.
 * 
 * Adds standard model parameters to handle periodic tasks in addition 
 * to the parameters used by FreeRTOS.
 * 
 * @param pvJobCode Pointer to the function that implements the job.
 * @param pcName Descriptive name for the job.
 * @param usStackDepth Length of the stack for this task, expressed in words.
 * @param pvParameters Parameters passed into the job.
 * @param uxPriority Priority of the job (not used if MST-specific scheduling is used).
 * @param pxCreatedTask Pointer to the job handle.
 * @param xTaskPeriod Period of the task in milliseconds.
 * @param xTaskDeadline Relative deadline of the task in milliseconds.
 * @param xTaskPhase Phase of the task in milliseconds (shift from first release).
 * @param xTaskWCET Worst-case execution time of the task.
 * 
 * @return Handle to the created task.
 */
TaskHandle_t vMSTPeriodicTaskCreate(
    TaskFunction_t pvJobCode,
    const char *const pcName,
    uint16_t usStackDepth,
    void *pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t *pxCreatedTask,
    TickType_t xTaskPeriod,
    TickType_t xTaskDeadline,
    TickType_t xTaskPhase,
    TickType_t xTaskWCET
);

/**
 * @brief Creates a sporadic task.
 * 
 * Adds standard model parameters to handle sporadic tasks in addition 
 * to the parameters used by FreeRTOS.
 * 
 * @param pvJobCode Pointer to the function that implements the job.
 * @param pcName Descriptive name for the job.
 * @param usStackDepth Length of the stack for this task, expressed in words.
 * @param pvParameters Parameters passed into the job.
 * @param uxPriority Priority of the job (not used if MST-specific scheduling is used).
 * @param pxCreatedTask Pointer to the job handle.
 * @param xTaskInterarrivalTime Minimum spacing between releases in milliseconds.
 * @param xTaskDeadline Relative deadline of the task in milliseconds.
 * @param xTaskWCET Worst-case execution time of the task.
 * 
 * @return pdPASS if the task was created successfully
 * @return pdFAIL otherwise
 */
BaseType_t vMSTSporadicTaskCreate(
    TaskFunction_t pvJobCode,
    const char *const pcName,
    uint16_t usStackDepth,
    void *pvParameters,
    UBaseType_t uxPriority,
    TaskHandle_t *pxCreatedTask,
    TickType_t xTaskInterarrivalTime,
    TickType_t xTaskDeadline,
    TickType_t xTaskWCET
);

/**
 * @brief Runs a sporadic job if the interarrival time has passed.
 * 
 * @param pxTaskToRunHandle Handle to the task to be run.
 * 
 * @return pdFAIL if the task was never created.
 * @return pdPASS if the task ran successfully.
 */
BaseType_t vMSTSporadicTaskRun(TaskHandle_t *pxTaskToRunHandle);

#endif /* _MST_SCHEDULER_H */
