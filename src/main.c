#include <stdio.h>
#include <string.h>
#include "stm32f1xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "modbus_registers.h"

#define MODBUS_SLAVE_ID     1     // Dirección de este esclavo Modbus
#define RX_BUFFER_SIZE      64    // Buffer de recepción serie

// VECTOR GLOBAL DE REGISTROS MODBUS
// Compartido entre vStateMachineTask, vReadInputsTask y vSerialCommTask
volatile uint16_t g_modbus_registers[REG_TOTAL_COUNT] = {
    [REG_TIME_INT1]      = 25,     // 25s - NS: Verde,    EO: Rojo
    [REG_TIME_INT2]      = 3,      //  3s - NS: Amarillo, EO: Rojo
    [REG_TIME_INT3]      = 2,      //  2s - NS: Rojo,     EO: Rojo (Despeje / Todo Rojo)
    [REG_TIME_INT4]      = 25,     // 25s - NS: Rojo,     EO: Verde
    [REG_TIME_INT5]      = 3,      //  3s - NS: Rojo,     EO: Amarillo
    [REG_TIME_INT6]      = 2,      //  2s - NS: Rojo,     EO: Rojo (Despeje / Todo Rojo)
    [REG_FSM_STATE]      = 0,      // Estado inicial FSM
    [REG_LIGHTS_NS]      = 0,      // Apagado al inicio (se actualizará en arranque)
    [REG_LIGHTS_EO]      = 0,
    [REG_BATTERY_MV]     = 12600,  // 12.6V nominal inicial
    [REG_CURRENT_MA]     = 0,      // Corriente inicial
    [REG_INPUTS_DIGITAL] = INPUT_MAINS_220V, // Red 220V presente
    [REG_FAULT_FLAGS]    = 0,
    [REG_OP_MODE]        = 0       // 0: Modo Normal / FSM
};

void vHardwareInit(void);
void vHardwareSetLights(uint16_t lights_ns, uint16_t lights_eo);
uint16_t usModbusCRC(const uint8_t *pucFrame, uint16_t usLen);
void vModbusProcessFrame(const uint8_t *rxBuf, uint16_t len);

void vStateMachineTask(void *pvParameters);
void vReadInputsTask(void *pvParameters);
void vSerialCommTask(void *pvParameters);

volatile uint32_t ulIdleCount = 0;

int main(void) {
    SystemCoreClockUpdate();
    vHardwareInit();

    xTaskCreate(vStateMachineTask, "StateMachine", 256, NULL, 3, NULL);

    xTaskCreate(vReadInputsTask,   "ReadInputs",   192, NULL, 2, NULL);

    xTaskCreate(vSerialCommTask,   "SerialComm",   256, NULL, 2, NULL);

    vTaskStartScheduler();

    while (1);
    return 0;
}

// Controla la secuencia de los 2 semáforos leyendo los intervalos del vector
void vStateMachineTask(void *pvParameters) {
    (void)pvParameters;
    uint8_t currentState = 0;

    // Arranque seguro: Todo Rojo durante los primeros 2 segundos
    g_modbus_registers[REG_FSM_STATE] = 98; // 98 = Todo Rojo inicio
    g_modbus_registers[REG_LIGHTS_NS] = LIGHT_RED;
    g_modbus_registers[REG_LIGHTS_EO] = LIGHT_RED;
    vHardwareSetLights(LIGHT_RED, LIGHT_RED);
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        uint16_t mode = g_modbus_registers[REG_OP_MODE];

        /* --- MODO INTERMITENTE (AMARILLO DESTELLANTE) --- */
        if (mode == 1) {
            g_modbus_registers[REG_FSM_STATE] = 99;
            g_modbus_registers[REG_LIGHTS_NS] = LIGHT_YELLOW;
            g_modbus_registers[REG_LIGHTS_EO] = LIGHT_YELLOW;
            vHardwareSetLights(LIGHT_YELLOW, LIGHT_YELLOW);
            vTaskDelay(pdMS_TO_TICKS(500));

            g_modbus_registers[REG_LIGHTS_NS] = 0;
            g_modbus_registers[REG_LIGHTS_EO] = 0;
            vHardwareSetLights(0, 0);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        /* --- MODO TODO ROJO FORZADO --- */
        if (mode == 2) {
            g_modbus_registers[REG_FSM_STATE] = 98;
            g_modbus_registers[REG_LIGHTS_NS] = LIGHT_RED;
            g_modbus_registers[REG_LIGHTS_EO] = LIGHT_RED;
            vHardwareSetLights(LIGHT_RED, LIGHT_RED);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        /* --- MODO NORMAL: FSM DE 6 INTERVALOS (Pág 9 central_semaforo.pdf) --- */
        uint16_t duration_sec = 0;
        uint16_t lights_ns = 0;
        uint16_t lights_eo = 0;

        switch (currentState) {
            case 0: // Intervalo 1: NS Verde (+ Peatonal), EO Rojo
                duration_sec = g_modbus_registers[REG_TIME_INT1];
                lights_ns    = LIGHT_GREEN | LIGHT_PEDESTRIAN;
                lights_eo    = LIGHT_RED;
                break;

            case 1: // Intervalo 2: NS Amarillo, EO Rojo
                duration_sec = g_modbus_registers[REG_TIME_INT2];
                lights_ns    = LIGHT_YELLOW;
                lights_eo    = LIGHT_RED;
                break;

            case 2: // Intervalo 3: NS Rojo, EO Rojo (Despeje Todo Rojo)
                duration_sec = g_modbus_registers[REG_TIME_INT3];
                lights_ns    = LIGHT_RED;
                lights_eo    = LIGHT_RED;
                break;

            case 3: // Intervalo 4: NS Rojo, EO Verde (+ Peatonal)
                duration_sec = g_modbus_registers[REG_TIME_INT4];
                lights_ns    = LIGHT_RED;
                lights_eo    = LIGHT_GREEN | LIGHT_PEDESTRIAN;
                break;

            case 4: // Intervalo 5: NS Rojo, EO Amarillo
                duration_sec = g_modbus_registers[REG_TIME_INT5];
                lights_ns    = LIGHT_RED;
                lights_eo    = LIGHT_YELLOW;
                break;

            case 5: // Intervalo 6: NS Rojo, EO Rojo (Despeje Todo Rojo)
                duration_sec = g_modbus_registers[REG_TIME_INT6];
                lights_ns    = LIGHT_RED;
                lights_eo    = LIGHT_RED;
                break;

            default:
                currentState = 0;
                continue;
        }

        // Seguridad: garantizar duración mínima de 1 segundo
        if (duration_sec == 0) duration_sec = 1;

        // Actualizar registros Modbus del estado actual y lámparas
        g_modbus_registers[REG_FSM_STATE] = currentState;
        g_modbus_registers[REG_LIGHTS_NS] = lights_ns;
        g_modbus_registers[REG_LIGHTS_EO] = lights_eo;

        // Actuar sobre el hardware físico
        vHardwareSetLights(lights_ns, lights_eo);

        // Esperar el tiempo configurado para este intervalo (en segundos)
        // Se ejecuta en pasos de 1 segundo para responder a cambios de modo del maestro
        for (uint16_t s = 0; s < duration_sec; s++) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            // Si el maestro cambia el modo de operación, interrumpir ciclo inmediatamente
            if (g_modbus_registers[REG_OP_MODE] != 0) {
                break;
            }
        }

        // Avanzar al siguiente estado
        currentState = (currentState + 1) % 6;
    }
}

/* ========================================================================= */
/* TAREA 2: vReadInputsTask                                                  */
/* Lee entradas digitales/analógicas y actualiza el vector Modbus            */
/* ========================================================================= */
void vReadInputsTask(void *pvParameters) {
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        // 1. Leer entrada digital de pulsador peatonal (PA0 con pull-up)
        uint16_t inputs = 0;
        if ((GPIOA->IDR & GPIO_IDR_IDR0) == 0) {
            inputs |= INPUT_BUTTON_NS; // Botón presionado (activo bajo)
        }

        // Simulación de presencia de red 220V (por defecto activa)
        inputs |= INPUT_MAINS_220V;
        g_modbus_registers[REG_INPUTS_DIGITAL] = inputs;

        // 2. Medición básica de batería (simulado/estimado en mV, ej. 12.4 V)
        // En etapas siguientes se conectará al canal ADC correspondiente
        uint16_t bat_mv = g_modbus_registers[REG_BATTERY_MV];
        if (!(inputs & INPUT_MAINS_220V)) {
            // Si no hay 220V, batería se descarga lentamente
            if (bat_mv > 10500) bat_mv -= 1;
        } else {
            bat_mv = 12600; // Cargador activo
        }
        g_modbus_registers[REG_BATTERY_MV] = bat_mv;

        // 3. Estimación/Medición de corriente según luces activas
        // Cada lámpara encendida consume aprox ~200mA
        uint16_t activeLamps = 0;
        uint16_t ns = g_modbus_registers[REG_LIGHTS_NS];
        uint16_t eo = g_modbus_registers[REG_LIGHTS_EO];

        for (int i = 0; i < 4; i++) {
            if (ns & (1 << i)) activeLamps++;
            if (eo & (1 << i)) activeLamps++;
        }
        g_modbus_registers[REG_CURRENT_MA] = activeLamps * 200;

        // 4. Verificación de banderas de falla
        uint16_t faults = 0;
        if (bat_mv < 11000) {
            faults |= (1 << 1); // Alarma: Batería baja
        }
        g_modbus_registers[REG_FAULT_FLAGS] = faults;

        // Período de muestreo: 100 ms
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));
    }
}

/* ========================================================================= */
/* TAREA 3: vSerialCommTask                                                  */
/* Atiende peticiones Modbus RTU (Funciones 0x03 y 0x06) desde el Maestro    */
/* ========================================================================= */
void vSerialCommTask(void *pvParameters) {
    (void)pvParameters;
    uint8_t rxBuffer[RX_BUFFER_SIZE];
    uint16_t rxIndex = 0;

    while (1) {
        // Leer bytes disponibles de USART1 (no bloqueante para el planificador)
        while ((USART1->SR & USART_SR_RXNE) != 0) {
            uint8_t byte = (uint8_t)(USART1->DR & 0xFF);
            if (rxIndex < RX_BUFFER_SIZE) {
                rxBuffer[rxIndex++] = byte;
            }
        }

        // Si se recibieron bytes, esperar pequeña pausa entre caracteres (silencio Modbus t3.5)
        if (rxIndex >= 8) {
            // Verificar CRC y procesar trama Modbus RTU
            uint16_t calculatedCRC = usModbusCRC(rxBuffer, rxIndex - 2);
            uint16_t receivedCRC   = rxBuffer[rxIndex - 2] | (rxBuffer[rxIndex - 1] << 8);

            if (calculatedCRC == receivedCRC && rxBuffer[0] == MODBUS_SLAVE_ID) {
                vModbusProcessFrame(rxBuffer, rxIndex);
            }
            rxIndex = 0; // Reiniciar buffer tras procesar
        } else if (rxIndex > 0) {
            // Espera breve para dar tiempo a que lleguen los bytes restantes de la trama
            vTaskDelay(pdMS_TO_TICKS(5));
            if ((USART1->SR & USART_SR_RXNE) == 0) {
                // Si venció el tiempo y no llegó una trama completa, descartar
                if (rxIndex < 8) {
                    rxIndex = 0;
                }
            }
        }

        // Tarea duerme 10 ms para ceder CPU si no hay datos entrantes
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* ========================================================================= */
/* PROCESADOR DE TRAMAS MODBUS RTU (0x03: Leer, 0x06: Escribir)             */
/* ========================================================================= */
void vModbusProcessFrame(const uint8_t *rxBuf, uint16_t len) {
    (void)len;
    uint8_t functionCode = rxBuf[1];
    uint8_t txBuf[RX_BUFFER_SIZE];
    uint16_t txLen = 0;

    if (functionCode == 0x03) {
        // Función 0x03: Leer Holding Registers
        uint16_t startAddr = (rxBuf[2] << 8) | rxBuf[3];
        uint16_t regCount  = (rxBuf[4] << 8) | rxBuf[5];

        if (startAddr + regCount <= REG_TOTAL_COUNT) {
            txBuf[0] = MODBUS_SLAVE_ID;
            txBuf[1] = 0x03;
            txBuf[2] = (uint8_t)(regCount * 2); // Cantidad de bytes de datos
            txLen = 3;

            for (uint16_t i = 0; i < regCount; i++) {
                uint16_t val = g_modbus_registers[startAddr + i];
                txBuf[txLen++] = (uint8_t)(val >> 8);
                txBuf[txLen++] = (uint8_t)(val & 0xFF);
            }

            uint16_t crc = usModbusCRC(txBuf, txLen);
            txBuf[txLen++] = (uint8_t)(crc & 0xFF);
            txBuf[txLen++] = (uint8_t)(crc >> 8);
        }
    } else if (functionCode == 0x06) {
        // Función 0x06: Escribir un Registro Simple (ej. modificar intervalo de tiempo o modo)
        uint16_t regAddr = (rxBuf[2] << 8) | rxBuf[3];
        uint16_t regVal  = (rxBuf[4] << 8) | rxBuf[5];

        if (regAddr < REG_TOTAL_COUNT) {
            g_modbus_registers[regAddr] = regVal;

            // La respuesta Modbus RTU a la función 0x06 es el mismo eco de la petición
            memcpy(txBuf, rxBuf, 8);
            txLen = 8;
        }
    }

    // Transmitir respuesta serie si se generó
    for (uint16_t i = 0; i < txLen; i++) {
        while ((USART1->SR & USART_SR_TXE) == 0);
        USART1->DR = txBuf[i];
    }
}

/* ========================================================================= */
/* CÁLCULO DE CRC16 MODBUS (Polinomio 0xA001)                                */
/* ========================================================================= */
uint16_t usModbusCRC(const uint8_t *pucFrame, uint16_t usLen) {
    uint16_t usCRC = 0xFFFF;
    while (usLen--) {
        usCRC ^= *pucFrame++;
        for (int i = 0; i < 8; i++) {
            if (usCRC & 0x0001) {
                usCRC = (usCRC >> 1) ^ 0xA001;
            } else {
                usCRC >>= 1;
            }
        }
    }
    return usCRC;
}

/* ========================================================================= */
/* CONTROL DE HARDWARE Y GPIO                                                */
/* ========================================================================= */
void vHardwareInit(void) {
    // 1. Habilitar Clocks de periféricos: GPIOA, GPIOB, GPIOC, USART1
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | 
                    RCC_APB2ENR_IOPCEN | RCC_APB2ENR_USART1EN;

    // 2. Configurar PC13 (LED onboard de Blue Pill) como salida push-pull
    GPIOC->CRH &= ~GPIO_CRH_CNF13;
    GPIOC->CRH |= GPIO_CRH_MODE13_0; // 10MHz output
    GPIOC->ODR |= GPIO_ODR_ODR13;    // Apagado inicial (activo en bajo)

    // 3. Configurar GPIOB para las 8 lámparas (PB0, PB1, PB10..PB15) como salida
    // PB0, PB1 (Norte-Sur: Rojo, Amarillo)
    GPIOB->CRL &= ~(GPIO_CRL_CNF0 | GPIO_CRL_CNF1);
    GPIOB->CRL |= (GPIO_CRL_MODE0_1 | GPIO_CRL_MODE1_1);

    // PB10..PB15 (Norte-Sur: Verde, Peatonal; Este-Oeste: Rojo, Amarillo, Verde, Peatonal)
    GPIOB->CRH &= ~(GPIO_CRH_CNF10 | GPIO_CRH_CNF11 | GPIO_CRH_CNF12 |
                    GPIO_CRH_CNF13 | GPIO_CRH_CNF14 | GPIO_CRH_CNF15);
    GPIOB->CRH |= (GPIO_CRH_MODE10_1 | GPIO_CRH_MODE11_1 | GPIO_CRH_MODE12_1 |
                   GPIO_CRH_MODE13_1 | GPIO_CRH_MODE14_1 | GPIO_CRH_MODE15_1);

    // 4. Configurar PA0 como entrada con Pull-up (Pulsador peatonal)
    GPIOA->CRL &= ~(GPIO_CRL_CNF0 | GPIO_CRL_MODE0);
    GPIOA->CRL |= GPIO_CRL_CNF0_1; // Input pull-up/pull-down
    GPIOA->ODR |= GPIO_ODR_ODR0;   // Pull-up

    // 5. Configurar USART1: PA9 (TX alternate push-pull), PA10 (RX input pull-up)
    GPIOA->CRH &= ~(GPIO_CRH_CNF9 | GPIO_CRH_MODE9 | GPIO_CRH_CNF10 | GPIO_CRH_MODE10);
    GPIOA->CRH |= (GPIO_CRH_CNF9_1 | GPIO_CRH_MODE9_0 | GPIO_CRH_MODE9_1 | GPIO_CRH_CNF10_1);
    GPIOA->ODR |= GPIO_ODR_ODR10;

    USART1->BRR = SystemCoreClock / 115200; // 115200 baud
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_UE; // Habilitar TX, RX y USART
}

void vHardwareSetLights(uint16_t lights_ns, uint16_t lights_eo) {
    // Norte-Sur:
    // PB0: Rojo, PB1: Amarillo, PB10: Verde, PB11: Peatonal
    if (lights_ns & LIGHT_RED)        GPIOB->BSRR = GPIO_BSRR_BS0;  else GPIOB->BSRR = GPIO_BSRR_BR0;
    if (lights_ns & LIGHT_YELLOW)     GPIOB->BSRR = GPIO_BSRR_BS1;  else GPIOB->BSRR = GPIO_BSRR_BR1;
    if (lights_ns & LIGHT_GREEN)      GPIOB->BSRR = GPIO_BSRR_BS10; else GPIOB->BSRR = GPIO_BSRR_BR10;
    if (lights_ns & LIGHT_PEDESTRIAN) GPIOB->BSRR = GPIO_BSRR_BS11; else GPIOB->BSRR = GPIO_BSRR_BR11;

    // Este-Oeste:
    // PB12: Rojo, PB13: Amarillo, PB14: Verde, PB15: Peatonal
    if (lights_eo & LIGHT_RED)        GPIOB->BSRR = GPIO_BSRR_BS12; else GPIOB->BSRR = GPIO_BSRR_BR12;
    if (lights_eo & LIGHT_YELLOW)     GPIOB->BSRR = GPIO_BSRR_BS13; else GPIOB->BSRR = GPIO_BSRR_BR13;
    if (lights_eo & LIGHT_GREEN)      GPIOB->BSRR = GPIO_BSRR_BS14; else GPIOB->BSRR = GPIO_BSRR_BR14;
    if (lights_eo & LIGHT_PEDESTRIAN) GPIOB->BSRR = GPIO_BSRR_BS15; else GPIOB->BSRR = GPIO_BSRR_BR15;

    // Conmutar LED PC13 para indicar actividad (Heartbeat)
    GPIOC->ODR ^= GPIO_ODR_ODR13;
}

/* Hook de Idle requerido por FreeRTOSConfig.h */
void vApplicationIdleHook(void) {
    ulIdleCount++;
}
