#include <stdio.h>
#include <string.h>
#include "stm32f1xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "modbus_registers.h"

#define MODBUS_SLAVE_ID     1     // Dirección de este esclavo Modbus
#define RX_BUFFER_SIZE      64    // Buffer de recepción serie

// Cola de recepción de bytes por interrupción
static QueueHandle_t xUartRxQueue = NULL;

volatile uint16_t g_modbus_registers[REG_TOTAL_COUNT] = {
    [REG_TIME_INT1]      = 25,     // 25s - NS: Verde,    EO: Rojo
    [REG_TIME_INT2]      = 3,      //  3s - NS: Amarillo, EO: Rojo
    [REG_TIME_INT3]      = 2,      //  2s - NS: Rojo,     EO: Rojo (Despeje / Todo Rojo)
    [REG_TIME_INT4]      = 25,     // 25s - NS: Rojo,     EO: Verde
    [REG_TIME_INT5]      = 3,      //  3s - NS: Rojo,     EO: Amarillo
    [REG_TIME_INT6]      = 2,      //  2s - NS: Rojo,     EO: Rojo (Despeje / Todo Rojo)
    [REG_TIME_INT7]      = 0,      //  0s - NS: Amarillo, EO: Amarillo (Intermitente ON)
    [REG_TIME_INT8]      = 0,      //  0s - NS: Off,      EO: Off      (Intermitente OFF)
    [REG_FSM_STATE]      = 0,      // Estado inicial FSM
    [REG_LIGHTS_NS]      = 0,      // Apagado al inicio (se actualizará en arranque)
    [REG_LIGHTS_EO]      = 0,
    [REG_BATTERY_MV]        = 12600,  // 12.6V nominal inicial
    [REG_CURRENT_MA]        = 0,      // Corriente medida desde el potenciómetro en PB1
    [REG_INPUTS_DIGITAL]    = INPUT_MAINS_220V, // Red 220V presente
    [REG_FAULT_FLAGS]       = 0,
    [REG_LDR_MV]            = 500,    // 500 mV inicial (Luz de día)
    [REG_LDR_THRESHOLD_MV]  = 2000,   // 2000 mV umbral por defecto (conmutable por Maestro)
    [REG_OP_MODE]           = OP_MODE_AUTO_LDR, // Modo Auto LDR por defecto
    [REG_LAMP_CURRENT_MA]   = 200,    // 200 mA nominal por lámpara activa
    [REG_CURRENT_TOL_PCT]   = 35,     // 35% de tolerancia admisible de corriente
    [REG_EXPECTED_CURR_MA]  = 400     // Corriente esperada calculada
};

void vHardwareInit(void);
uint16_t usAdcReadChannel(uint8_t channel);
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

    // Crear cola para recepción serie por interrupción
    xUartRxQueue = xQueueCreate(RX_BUFFER_SIZE, sizeof(uint8_t));
    if (xUartRxQueue == NULL) {
        while (1); // Error al reservar memoria para la cola
    }

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
    uint8_t zeroCount = 0;

    // Arranque seguro: Todo Rojo durante los primeros 2 segundos
    g_modbus_registers[REG_FSM_STATE] = 2; // Estado 2 = Todo Rojo (Despeje)
    g_modbus_registers[REG_LIGHTS_NS] = LIGHT_RED;
    g_modbus_registers[REG_LIGHTS_EO] = LIGHT_RED;
    vHardwareSetLights(LIGHT_RED, LIGHT_RED);
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        uint16_t opMode = g_modbus_registers[REG_OP_MODE];
        uint16_t inputs = g_modbus_registers[REG_INPUTS_DIGITAL];

        // MODO 1: Destellante manual O MODO 3 (Auto LDR) cuando oscurece (Noche)
        if (opMode == OP_MODE_DESTELLANTE || (opMode == OP_MODE_AUTO_LDR && (inputs & INPUT_LDR_NIGHT))) {
            g_modbus_registers[REG_FSM_STATE] = 99; // 99 = Destellante nocturno / precaución
            g_modbus_registers[REG_LIGHTS_NS] = LIGHT_YELLOW;
            g_modbus_registers[REG_LIGHTS_EO] = LIGHT_YELLOW;
            vHardwareSetLights(LIGHT_YELLOW, LIGHT_YELLOW);

            // 500 ms encendido
            for (int i = 0; i < 5; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
                uint16_t m = g_modbus_registers[REG_OP_MODE];
                uint16_t in = g_modbus_registers[REG_INPUTS_DIGITAL];
                if (m != opMode || (m == OP_MODE_AUTO_LDR && !(in & INPUT_LDR_NIGHT))) break;
            }

            g_modbus_registers[REG_LIGHTS_NS] = 0;
            g_modbus_registers[REG_LIGHTS_EO] = 0;
            vHardwareSetLights(0, 0);

            // 500 ms apagado
            for (int i = 0; i < 5; i++) {
                vTaskDelay(pdMS_TO_TICKS(100));
                uint16_t m = g_modbus_registers[REG_OP_MODE];
                uint16_t in = g_modbus_registers[REG_INPUTS_DIGITAL];
                if (m != opMode || (m == OP_MODE_AUTO_LDR && !(in & INPUT_LDR_NIGHT))) break;
            }
            continue;
        }

        // MODO 2: Todo Rojo forzado (Emergencia / Seguridad)
        if (opMode == OP_MODE_TODO_ROJO) {
            g_modbus_registers[REG_FSM_STATE] = 98; // 98 = Todo Rojo forzado
            g_modbus_registers[REG_LIGHTS_NS] = LIGHT_RED;
            g_modbus_registers[REG_LIGHTS_EO] = LIGHT_RED;
            vHardwareSetLights(LIGHT_RED, LIGHT_RED);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // MODO 0: Normal FSM (o Auto LDR durante el día)
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

            case 6: // Intervalo 7: NS Amarillo, EO Amarillo (Intermitente ON)
                duration_sec = g_modbus_registers[REG_TIME_INT7];
                lights_ns    = LIGHT_YELLOW;
                lights_eo    = LIGHT_YELLOW;
                break;

            case 7: // Intervalo 8: NS Off, EO Off (Intermitente OFF)
                duration_sec = g_modbus_registers[REG_TIME_INT8];
                lights_ns    = 0;
                lights_eo    = 0;
                break;

            default:
                currentState = 0;
                continue;
        }

        // Si la duración es mayor a 0, ejecuta el estado
        if (duration_sec > 0) {
            zeroCount = 0;

            // Actualizar registros Modbus del estado actual y lámparas
            g_modbus_registers[REG_FSM_STATE] = currentState;
            g_modbus_registers[REG_LIGHTS_NS] = lights_ns;
            g_modbus_registers[REG_LIGHTS_EO] = lights_eo;

            // Actuar sobre el hardware físico
            vHardwareSetLights(lights_ns, lights_eo);

            // Esperar la duración del intervalo en segundos con muestreo cada 100 ms para respuesta inmediata
            for (uint16_t s = 0; s < duration_sec; s++) {
                for (int sub = 0; sub < 10; sub++) {
                    vTaskDelay(pdMS_TO_TICKS(100));

                    // Si cambia el modo o se detecta noche en Auto LDR, salir inmediatamente
                    uint16_t curMode = g_modbus_registers[REG_OP_MODE];
                    uint16_t curIn   = g_modbus_registers[REG_INPUTS_DIGITAL];
                    if (curMode == OP_MODE_DESTELLANTE || curMode == OP_MODE_TODO_ROJO ||
                        (curMode == OP_MODE_AUTO_LDR && (curIn & INPUT_LDR_NIGHT))) {
                        s = duration_sec; // Salir de inmediato
                        break;
                    }

                    // Si el maestro pone en 0 el tiempo del estado actual en caliente, salir
                    if (g_modbus_registers[currentState] == 0) {
                        s = duration_sec;
                        break;
                    }
                }
            }
        } else {
            // Si todos los estados estuvieran en 0, ceder CPU para evitar bucle activo
            zeroCount++;
            if (zeroCount >= 8) {
                vTaskDelay(pdMS_TO_TICKS(50));
                zeroCount = 0;
            }
        }

        // Avanzar cíclicamente al siguiente de los 8 estados
        currentState = (currentState + 1) % 8;
    }
}

// Lee entradas digitales/analógicas (LDR PB0, Pote Corriente PB1, Batería, 220V) y actualiza el vector Modbus
void vReadInputsTask(void *pvParameters) {
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        // 1. Lectura de sensor LDR en PB0 (ADC1 Canal 8)
        uint32_t adcSumLdr = 0;
        for (int i = 0; i < 4; i++) {
            adcSumLdr += usAdcReadChannel(8);
        }
        uint16_t adcRawLdr = (uint16_t)(adcSumLdr / 4);
        uint16_t ldr_mv = (uint16_t)(((uint32_t)adcRawLdr * 3300UL) / 4095UL);
        g_modbus_registers[REG_LDR_MV] = ldr_mv;

        // 2. Evaluación de condición Día / Noche según umbral configurable
        uint16_t threshold_mv = g_modbus_registers[REG_LDR_THRESHOLD_MV];
        if (threshold_mv == 0) {
            threshold_mv = 2000; // Umbral por defecto (2.0V)
            g_modbus_registers[REG_LDR_THRESHOLD_MV] = threshold_mv;
        }

        uint16_t inputs = 0;
        inputs |= INPUT_MAINS_220V; // Presencia red 220V
        if (ldr_mv >= threshold_mv) {
            inputs |= INPUT_LDR_NIGHT; // Noche detectada (tensión alta por alta resistencia de LDR en oscuridad)
        } else {
            inputs &= ~INPUT_LDR_NIGHT; // Luz de día
        }
        g_modbus_registers[REG_INPUTS_DIGITAL] = inputs;

        // 3. Lectura de potenciómetro de simulación de corriente en PB1 (ADC1 Canal 9)
        uint32_t adcSumPot = 0;
        for (int i = 0; i < 4; i++) {
            adcSumPot += usAdcReadChannel(9);
        }
        uint16_t adcRawPot = (uint16_t)(adcSumPot / 4);
        // Escala del potenciómetro: 0V a 3.3V mapeado a 0 mA - 1200 mA
        uint16_t measured_ma = (uint16_t)(((uint32_t)adcRawPot * 1200UL) / 4095UL);
        g_modbus_registers[REG_CURRENT_MA] = measured_ma;

        // 4. Medición básica de batería (simulado/estimado en mV, ej. 12.4 V)
        uint16_t bat_mv = g_modbus_registers[REG_BATTERY_MV];
        if (!(inputs & INPUT_MAINS_220V)) {
            if (bat_mv > 10500) bat_mv -= 1;
        } else {
            bat_mv = 12600; // Cargador activo
        }
        g_modbus_registers[REG_BATTERY_MV] = bat_mv;

        // 5. Diagnóstico de corriente esperada y detección de fallas de lámparas
        uint16_t activeLamps = 0;
        uint16_t ns = g_modbus_registers[REG_LIGHTS_NS];
        uint16_t eo = g_modbus_registers[REG_LIGHTS_EO];
        for (int i = 0; i < 4; i++) {
            if (ns & (1 << i)) activeLamps++;
            if (eo & (1 << i)) activeLamps++;
        }

        uint16_t lampNominal = g_modbus_registers[REG_LAMP_CURRENT_MA];
        if (lampNominal == 0) {
            lampNominal = 200;
            g_modbus_registers[REG_LAMP_CURRENT_MA] = lampNominal;
        }

        uint16_t expectedCurrent = activeLamps * lampNominal;
        g_modbus_registers[REG_EXPECTED_CURR_MA] = expectedCurrent;

        uint16_t tolPct = g_modbus_registers[REG_CURRENT_TOL_PCT];
        if (tolPct == 0) {
            tolPct = 35;
            g_modbus_registers[REG_CURRENT_TOL_PCT] = tolPct;
        }

        uint16_t iMin = (uint16_t)(((uint32_t)expectedCurrent * (100 - tolPct)) / 100);
        uint16_t iMax = (uint16_t)(((uint32_t)expectedCurrent * (100 + tolPct)) / 100);

        // 6. Evaluación de banderas de falla
        uint16_t faults = 0;
        if (bat_mv < 11000) {
            faults |= FAULT_BATTERY_LOW; // Alarma: Batería baja (<11.0V)
        }

        if (activeLamps > 0) {
            if (measured_ma < iMin) {
                faults |= FAULT_LAMP_BURNED; // Falla: Lámpara quemada / subcorriente
            } else if (measured_ma > iMax) {
                faults |= FAULT_OVERCURRENT; // Falla: Sobrecorriente / cortocircuito
            }
        } else {
            if (measured_ma > 50) {
                faults |= FAULT_LAMP_BURNED; // Falla: Corriente indebida con lámparas apagadas
            }
        }
        g_modbus_registers[REG_FAULT_FLAGS] = faults;

        // Período de muestreo: 100 ms
        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));
    }
}

// Atiende peticiones Modbus RTU (Funciones 0x03 y 0x06) desde el Maestro
void vSerialCommTask(void *pvParameters) {
    (void)pvParameters;
    uint8_t rxBuffer[RX_BUFFER_SIZE];
    uint16_t rxIndex = 0;
    uint8_t byte;

    while (1) {
        // Si el buffer está vacío, se bloquea indefinidamente hasta recibir el 1er byte (0% CPU).
        // Si ya hay datos en curso, se espera hasta 5 ms (silencio t3.5 fin de trama Modbus RTU).
        TickType_t xWaitTime = (rxIndex == 0) ? portMAX_DELAY : pdMS_TO_TICKS(5);

        if (xQueueReceive(xUartRxQueue, &byte, xWaitTime) == pdPASS) {
            if (rxIndex < RX_BUFFER_SIZE) {
                rxBuffer[rxIndex++] = byte;
            }
        } else {
            // El timeout de 5 ms venció: fin de trama detectado por silencio de la línea
            if (rxIndex >= 8) {
                uint16_t calculatedCRC = usModbusCRC(rxBuffer, rxIndex - 2);
                uint16_t receivedCRC   = rxBuffer[rxIndex - 2] | (rxBuffer[rxIndex - 1] << 8);

                if (calculatedCRC == receivedCRC && rxBuffer[0] == MODBUS_SLAVE_ID) {
                    vModbusProcessFrame(rxBuffer, rxIndex);
                }
            }
            // Reiniciar índice para esperar la siguiente trama
            rxIndex = 0;
        }
    }
}

// PROCESADOR DE TRAMAS MODBUS RTU (0x03: Leer, 0x06: Escribir)
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

// CÁLCULO DE CRC16 MODBUS (Polinomio 0xA001)
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

// CONTROL DE HARDWARE Y GPIO
void vHardwareInit(void) {
    // 1. Habilitar Clocks de periféricos: GPIOA, GPIOB, GPIOC, AFIO, USART1, ADC1
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN |
                    RCC_APB2ENR_IOPCEN | RCC_APB2ENR_AFIOEN | RCC_APB2ENR_USART1EN |
                    RCC_APB2ENR_ADC1EN;

    // Prescaler del ADC: APB2 / 6 = 72MHz / 6 = 12MHz (máx permitido 14MHz)
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_ADCPRE) | RCC_CFGR_ADCPRE_DIV6;

    // Liberar PB4 (por defecto asignado a JTAG NJTRST) manteniendo SWD activo para debug/flasheo
    AFIO->MAPR = (AFIO->MAPR & ~AFIO_MAPR_SWJ_CFG) | AFIO_MAPR_SWJ_CFG_JTAGDISABLE;

    // Configurar PB0 y PB1 como entradas analógicas:
    // PB0 (ADC1_IN8): Sensor LDR (Divisor 3.3V -- 10k -- PB0 -- LDR -- GND)
    // PB1 (ADC1_IN9): Potenciómetro simulación corriente (3.3V -- POTE -- GND)
    GPIOB->CRL &= ~(GPIO_CRL_CNF0 | GPIO_CRL_MODE0 | GPIO_CRL_CNF1 | GPIO_CRL_MODE1);

    // Calibración e inicialización de ADC1 con disparador por software (SWSTART)
    ADC1->CR2 |= ADC_CR2_ADON | ADC_CR2_EXTTRIG | (0x7 << ADC_CR2_EXTSEL_Pos);
    for (volatile int i = 0; i < 2000; i++); // Estabilización
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    while (ADC1->CR2 & ADC_CR2_RSTCAL);
    ADC1->CR2 |= ADC_CR2_CAL;
    while (ADC1->CR2 & ADC_CR2_CAL);

    // Tiempo de muestreo para canal 8 (PB0) y canal 9 (PB1): 239.5 ciclos (máxima precisión)
    ADC1->SMPR2 |= (0x7 << ADC_SMPR2_SMP8_Pos) | (0x7 << ADC_SMPR2_SMP9_Pos);

    // 2. Configurar PC13 (LED onboard de Blue Pill) como salida push-pull
    GPIOC->CRH &= ~GPIO_CRH_CNF13;
    GPIOC->CRH |= GPIO_CRH_MODE13_0; // 10MHz output
    GPIOC->ODR |= GPIO_ODR_ODR13;    // Apagado inicial (activo en bajo)

    // 3. Configurar GPIOB para los semáforos como salida Push-Pull (2 MHz):
    // Fase 1: PB4 (Rojo), PB5 (Amarillo), PB6 (Verde)
    // Fase 2: PB7 (Rojo)
    GPIOB->CRL &= ~(GPIO_CRL_CNF4 | GPIO_CRL_MODE4 |
                    GPIO_CRL_CNF5 | GPIO_CRL_MODE5 |
                    GPIO_CRL_CNF6 | GPIO_CRL_MODE6 |
                    GPIO_CRL_CNF7 | GPIO_CRL_MODE7);
    GPIOB->CRL |=  (GPIO_CRL_MODE4_1 | GPIO_CRL_MODE5_1 |
                    GPIO_CRL_MODE6_1 | GPIO_CRL_MODE7_1);

    // Fase 2: PB8 (Amarillo), PB9 (Verde)
    GPIOB->CRH &= ~(GPIO_CRH_CNF8 | GPIO_CRH_MODE8 |
                    GPIO_CRH_CNF9 | GPIO_CRH_MODE9);
    GPIOB->CRH |=  (GPIO_CRH_MODE8_1 | GPIO_CRH_MODE9_1);

    // 4. Configurar USART1: PA9 (TX alternate push-pull), PA10 (RX input pull-up)
    GPIOA->CRH &= ~(GPIO_CRH_CNF9 | GPIO_CRH_MODE9 | GPIO_CRH_CNF10 | GPIO_CRH_MODE10);
    GPIOA->CRH |= (GPIO_CRH_CNF9_1 | GPIO_CRH_MODE9_0 | GPIO_CRH_MODE9_1 | GPIO_CRH_CNF10_1);
    GPIOA->ODR |= GPIO_ODR_ODR10;

    USART1->BRR = SystemCoreClock / 115200; // 115200 baud
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    // Configurar prioridad e interrupción en NVIC (prioridad 6 para compatibilidad con FreeRTOS)
    NVIC_SetPriority(USART1_IRQn, 6);
    NVIC_EnableIRQ(USART1_IRQn);
}

/* Manejador de interrupción de recepción serie USART1 */
void USART1_IRQHandler(void) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    if (USART1->SR & USART_SR_RXNE) {
        uint8_t byte = (uint8_t)(USART1->DR & 0xFF);
        if (xUartRxQueue != NULL) {
            xQueueSendFromISR(xUartRxQueue, &byte, &xHigherPriorityTaskWoken);
        }
    }

    // Limpiar Overrun Error si se produce
    if (USART1->SR & USART_SR_ORE) {
        volatile uint32_t dummy = USART1->DR;
        (void)dummy;
    }

    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// LECTURA DE CANAL ANALÓGICO ADC1 CON DISPARADOR SWSTART Y TIMEOUT DE SEGURIDAD
uint16_t usAdcReadChannel(uint8_t channel) {
    ADC1->SQR3 = channel;
    ADC1->SR &= ~ADC_SR_EOC;
    ADC1->CR2 |= ADC_CR2_SWSTART;
    uint32_t timeout = 20000;
    while (!(ADC1->SR & ADC_SR_EOC) && --timeout);
    return (uint16_t)(ADC1->DR & 0x0FFF);
}

void vHardwareSetLights(uint16_t lights_ns, uint16_t lights_eo) {
    // Fase 1 (Norte-Sur):
    // PB4: Rojo, PB5: Amarillo, PB6: Verde
    if (lights_ns & LIGHT_RED)    GPIOB->BSRR = GPIO_BSRR_BS4; else GPIOB->BSRR = GPIO_BSRR_BR4;
    if (lights_ns & LIGHT_YELLOW) GPIOB->BSRR = GPIO_BSRR_BS5; else GPIOB->BSRR = GPIO_BSRR_BR5;
    if (lights_ns & LIGHT_GREEN)  GPIOB->BSRR = GPIO_BSRR_BS6; else GPIOB->BSRR = GPIO_BSRR_BR6;

    // Fase 2 (Este-Oeste):
    // PB7: Rojo, PB8: Amarillo, PB9: Verde
    if (lights_eo & LIGHT_RED)    GPIOB->BSRR = GPIO_BSRR_BS7; else GPIOB->BSRR = GPIO_BSRR_BR7;
    if (lights_eo & LIGHT_YELLOW) GPIOB->BSRR = GPIO_BSRR_BS8; else GPIOB->BSRR = GPIO_BSRR_BR8;
    if (lights_eo & LIGHT_GREEN)  GPIOB->BSRR = GPIO_BSRR_BS9; else GPIOB->BSRR = GPIO_BSRR_BR9;

    // Conmutar LED PC13 para indicar actividad (Heartbeat)
    GPIOC->ODR ^= GPIO_ODR_ODR13;
}

/* Hook de Idle requerido por FreeRTOSConfig.h */
void vApplicationIdleHook(void) {
    ulIdleCount++;
}

