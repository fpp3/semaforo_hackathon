#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ------------------- Sección 1: Configuración del Hardware ------------------ */
#define configCPU_CLOCK_HZ                     ( 72000000UL )
#define configTICK_RATE_HZ                     ( ( TickType_t ) 1000 ) // 1ms por tick

/* ------------------- Sección 2: Configuración del Kernel ------------------ */
#define configUSE_PREEMPTION                   1
#define configSUPPORT_DYNAMIC_ALLOCATION       1
#define configSUPPORT_STATIC_ALLOCATION        0
#define configTOTAL_HEAP_SIZE                  ( ( size_t ) ( 5 * 1024 ) ) // 5KB para el heap
#define configMAX_TASK_NAME_LEN                ( 16 )
#define configUSE_16_BIT_TICKS                 0

/* ------------------- Sección 3: Ganchos (Hooks) y Opciones de Depuración ------------------ */
#define configUSE_IDLE_HOOK                    0
#define configUSE_TICK_HOOK                    0
#define configCHECK_FOR_STACK_OVERFLOW         0 // Poner en 2 durante la depuración

/* ------------------- Sección 4: Inclusión de APIs ------------------ */
#define INCLUDE_vTaskDelay                     1

/* ------------------- Sección 5: Prioridades de Interrupción (¡MUY IMPORTANTE!) ------------------ */
// En los Cortex-M, una prioridad más baja se representa con un valor numérico más alto.
// Prioridad 0 es la más alta.
#define configKERNEL_INTERRUPT_PRIORITY        255 // La prioridad más baja posible
#define configMAX_SYSCALL_INTERRUPT_PRIORITY   191 // Prioridad 11. Las interrupciones con prioridad numérica inferior a esta no pueden llamar a la API de FreeRTOS.

/* ------------------- Sección 6: Mapeo de Handlers a CMSIS ------------------ */
/* Se asegura de que los nombres de las interrupciones del sistema que usa FreeRTOS
   se mapeen a los nombres definidos en el archivo de startup de CMSIS. */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */

