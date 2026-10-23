#include <stdio.h>
#include <string.h>
#include "stm32f1xx.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "modbus_registers.h"

#define MODBUS_SLAVE_ID     1     // Dirección de este esclavo Modbus
#define RX_BUFFER_SIZE      64    // Buffer de recepción serie

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

    xUartRxQueue = xQueueCreate(RX_BUFFER_SIZE, sizeof(uint8_t));
    if (xUartRxQueue == NULL) {
        while (1);
    }

    xTaskCreate(vStateMachineTask, "StateMachine", 256, NULL, 3, NULL);

    xTaskCreate(vReadInputsTask,   "ReadInputs",   192, NULL, 2, NULL);

    xTaskCreate(vSerialCommTask,   "SerialComm",   256, NULL, 2, NULL);

    vTaskStartScheduler();

    while (1);
    return 0;
}

void vStateMachineTask(void *pvParameters) {
    (void)pvParameters;
    uint8_t currentState = 0;
    uint8_t zeroCount = 0;

    g_modbus_registers[REG_FSM_STATE] = 2;
    g_modbus_registers[REG_LIGHTS_NS] = LIGHT_RED;
    g_modbus_registers[REG_LIGHTS_EO] = LIGHT_RED;
    vHardwareSetLights(LIGHT_RED, LIGHT_RED);
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        uint16_t duration_sec = 0;
        uint16_t lights_ns = 0;
        uint16_t lights_eo = 0;

        switch (currentState) {
            case 0:
                duration_sec = g_modbus_registers[REG_TIME_INT1];
                lights_ns    = LIGHT_GREEN | LIGHT_PEDESTRIAN;
                lights_eo    = LIGHT_RED;
                break;
            case 1:
                duration_sec = g_modbus_registers[REG_TIME_INT2];
                lights_ns    = LIGHT_YELLOW;
                lights_eo    = LIGHT_RED;
                break;
            case 2:
                duration_sec = g_modbus_registers[REG_TIME_INT3];
                lights_ns    = LIGHT_RED;
                lights_eo    = LIGHT_RED;
                break;
            case 3:
                duration_sec = g_modbus_registers[REG_TIME_INT4];
                lights_ns    = LIGHT_RED;
                lights_eo    = LIGHT_GREEN | LIGHT_PEDESTRIAN;
                break;
            case 4:
                duration_sec = g_modbus_registers[REG_TIME_INT5];
                lights_ns    = LIGHT_RED;
                lights_eo    = LIGHT_YELLOW;
                break;
            case 5:
                duration_sec = g_modbus_registers[REG_TIME_INT6];
                lights_ns    = LIGHT_RED;
                lights_eo    = LIGHT_RED;
                break;
            case 6:
                duration_sec = g_modbus_registers[REG_TIME_INT7];
                lights_ns    = LIGHT_YELLOW;
                lights_eo    = LIGHT_YELLOW;
                break;
            case 7:
                duration_sec = g_modbus_registers[REG_TIME_INT8];
                lights_ns    = 0;
                lights_eo    = 0;
                break;
            default:
                currentState = 0;
                continue;
        }

        if (duration_sec > 0) {
            zeroCount = 0;
            g_modbus_registers[REG_FSM_STATE] = currentState;
            g_modbus_registers[REG_LIGHTS_NS] = lights_ns;
            g_modbus_registers[REG_LIGHTS_EO] = lights_eo;
            vHardwareSetLights(lights_ns, lights_eo);
            for (uint16_t s = 0; s < duration_sec; s++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (g_modbus_registers[currentState] == 0) {
                    break;
                }
            }
        } else {
            zeroCount++;
            if (zeroCount >= 8) {
                vTaskDelay(pdMS_TO_TICKS(50));
                zeroCount = 0;
            }
        }
        currentState = (currentState + 1) % 8;
    }
}

void vReadInputsTask(void *pvParameters) {
    (void)pvParameters;
    TickType_t xLastWakeTime = xTaskGetTickCount();

    while (1) {
        uint32_t adcSumLdr = 0;
        for (int i = 0; i < 4; i++) {
            adcSumLdr += usAdcReadChannel(8);
        }
        uint16_t adcRawLdr = (uint16_t)(adcSumLdr / 4);
        uint16_t ldr_mv = (uint16_t)(((uint32_t)adcRawLdr * 3300UL) / 4095UL);
        g_modbus_registers[REG_LDR_MV] = ldr_mv;

        uint16_t threshold_mv = g_modbus_registers[REG_LDR_THRESHOLD_MV];
        if (threshold_mv == 0) {
            threshold_mv = 2000; // Umbral por defecto (2.0V)
            g_modbus_registers[REG_LDR_THRESHOLD_MV] = threshold_mv;
        }

        uint16_t inputs = 0;
        inputs |= INPUT_MAINS_220V;
        if (ldr_mv >= threshold_mv) {
            inputs |= INPUT_LDR_NIGHT;
        } else {
            inputs &= ~INPUT_LDR_NIGHT;
        }
        g_modbus_registers[REG_INPUTS_DIGITAL] = inputs;

        uint32_t adcSumPot = 0;
        for (int i = 0; i < 4; i++) {
            adcSumPot += usAdcReadChannel(9);
        }
        uint16_t adcRawPot = (uint16_t)(adcSumPot / 4);
        uint16_t measured_ma = (uint16_t)(((uint32_t)adcRawPot * 1200UL) / 4095UL);
        g_modbus_registers[REG_CURRENT_MA] = measured_ma;

        uint16_t bat_mv = g_modbus_registers[REG_BATTERY_MV];
        if (!(inputs & INPUT_MAINS_220V)) {
            if (bat_mv > 10500) bat_mv -= 1;
        } else {
            bat_mv = 12600; // Cargador activo
        }
        g_modbus_registers[REG_BATTERY_MV] = bat_mv;

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

        uint16_t faults = 0;
        if (bat_mv < 11000) {
            faults |= FAULT_BATTERY_LOW;
        }

        if (activeLamps > 0) {
            if (measured_ma < iMin) {
                faults |= FAULT_LAMP_BURNED;
            } else if (measured_ma > iMax) {
                faults |= FAULT_OVERCURRENT;
            }
        } else {
            if (measured_ma > 50) {
                faults |= FAULT_LAMP_BURNED;
            }
        }
        g_modbus_registers[REG_FAULT_FLAGS] = faults;

        vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(100));
    }
}

void vSerialCommTask(void *pvParameters) {
    (void)pvParameters;
    uint8_t rxBuffer[RX_BUFFER_SIZE];
    uint16_t rxIndex = 0;
    uint8_t byte;

    while (1) {
        TickType_t xWaitTime = (rxIndex == 0) ? portMAX_DELAY : pdMS_TO_TICKS(5);

        if (xQueueReceive(xUartRxQueue, &byte, xWaitTime) == pdPASS) {
            if (rxIndex < RX_BUFFER_SIZE) {
                rxBuffer[rxIndex++] = byte;
            }
        } else {
            if (rxIndex >= 8) {
                uint16_t calculatedCRC = usModbusCRC(rxBuffer, rxIndex - 2);
                uint16_t receivedCRC   = rxBuffer[rxIndex - 2] | (rxBuffer[rxIndex - 1] << 8);

                if (calculatedCRC == receivedCRC && rxBuffer[0] == MODBUS_SLAVE_ID) {
                    vModbusProcessFrame(rxBuffer, rxIndex);
                }
            }
            rxIndex = 0;
        }
    }
}

void vModbusProcessFrame(const uint8_t *rxBuf, uint16_t len) {
    (void)len;
    uint8_t functionCode = rxBuf[1];
    uint8_t txBuf[RX_BUFFER_SIZE];
    uint16_t txLen = 0;

    if (functionCode == 0x03) {
        uint16_t startAddr = (rxBuf[2] << 8) | rxBuf[3];
        uint16_t regCount  = (rxBuf[4] << 8) | rxBuf[5];

        if (startAddr + regCount <= REG_TOTAL_COUNT) {
            txBuf[0] = MODBUS_SLAVE_ID;
            txBuf[1] = 0x03;
            txBuf[2] = (uint8_t)(regCount * 2);
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
        uint16_t regAddr = (rxBuf[2] << 8) | rxBuf[3];
        uint16_t regVal  = (rxBuf[4] << 8) | rxBuf[5];

        if (regAddr < REG_TOTAL_COUNT) {
            g_modbus_registers[regAddr] = regVal;
            memcpy(txBuf, rxBuf, 8);
            txLen = 8;
        }
    }

    for (uint16_t i = 0; i < txLen; i++) {
        while ((USART1->SR & USART_SR_TXE) == 0);
        USART1->DR = txBuf[i];
    }
}

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

void vHardwareInit(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN |
                    RCC_APB2ENR_IOPCEN | RCC_APB2ENR_AFIOEN | RCC_APB2ENR_USART1EN |
                    RCC_APB2ENR_ADC1EN;

    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_ADCPRE) | RCC_CFGR_ADCPRE_DIV6;
    AFIO->MAPR = (AFIO->MAPR & ~AFIO_MAPR_SWJ_CFG) | AFIO_MAPR_SWJ_CFG_JTAGDISABLE;
    GPIOB->CRL &= ~(GPIO_CRL_CNF0 | GPIO_CRL_MODE0 | GPIO_CRL_CNF1 | GPIO_CRL_MODE1);
    ADC1->CR2 |= ADC_CR2_ADON | ADC_CR2_EXTTRIG | (0x7 << ADC_CR2_EXTSEL_Pos);
    for (volatile int i = 0; i < 2000; i++); // Estabilización
    ADC1->CR2 |= ADC_CR2_RSTCAL;
    while (ADC1->CR2 & ADC_CR2_RSTCAL);
    ADC1->CR2 |= ADC_CR2_CAL;
    while (ADC1->CR2 & ADC_CR2_CAL);
    ADC1->SMPR2 |= (0x7 << ADC_SMPR2_SMP8_Pos) | (0x7 << ADC_SMPR2_SMP9_Pos);
    GPIOC->CRH &= ~GPIO_CRH_CNF13;
    GPIOC->CRH |= GPIO_CRH_MODE13_0;
    GPIOC->ODR |= GPIO_ODR_ODR13;
    GPIOB->CRL &= ~(GPIO_CRL_CNF4 | GPIO_CRL_MODE4 |
                    GPIO_CRL_CNF5 | GPIO_CRL_MODE5 |
                    GPIO_CRL_CNF6 | GPIO_CRL_MODE6 |
                    GPIO_CRL_CNF7 | GPIO_CRL_MODE7);
    GPIOB->CRL |=  (GPIO_CRL_MODE4_1 | GPIO_CRL_MODE5_1 |
                    GPIO_CRL_MODE6_1 | GPIO_CRL_MODE7_1);

    GPIOB->CRH &= ~(GPIO_CRH_CNF8 | GPIO_CRH_MODE8 |
                    GPIO_CRH_CNF9 | GPIO_CRH_MODE9);
    GPIOB->CRH |=  (GPIO_CRH_MODE8_1 | GPIO_CRH_MODE9_1);
    GPIOA->CRH &= ~(GPIO_CRH_CNF9 | GPIO_CRH_MODE9 | GPIO_CRH_CNF10 | GPIO_CRH_MODE10);
    GPIOA->CRH |= (GPIO_CRH_CNF9_1 | GPIO_CRH_MODE9_0 | GPIO_CRH_MODE9_1 | GPIO_CRH_CNF10_1);
    GPIOA->ODR |= GPIO_ODR_ODR10;

    USART1->BRR = SystemCoreClock / 115200;
    USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE | USART_CR1_UE;

    NVIC_SetPriority(USART1_IRQn, 6);
    NVIC_EnableIRQ(USART1_IRQn);
}

void USART1_IRQHandler(void) {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (USART1->SR & USART_SR_RXNE) {
        uint8_t byte = (uint8_t)(USART1->DR & 0xFF);
        if (xUartRxQueue != NULL) {
            xQueueSendFromISR(xUartRxQueue, &byte, &xHigherPriorityTaskWoken);
        }
    }

    if (USART1->SR & USART_SR_ORE) {
        volatile uint32_t dummy = USART1->DR;
        (void)dummy;
    }
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

uint16_t usAdcReadChannel(uint8_t channel) {
    ADC1->SQR3 = channel;
    ADC1->SR &= ~ADC_SR_EOC;
    ADC1->CR2 |= ADC_CR2_SWSTART;
    uint32_t timeout = 20000;
    while (!(ADC1->SR & ADC_SR_EOC) && --timeout);
    return (uint16_t)(ADC1->DR & 0x0FFF);
}

void vHardwareSetLights(uint16_t lights_ns, uint16_t lights_eo) {
    if (lights_ns & LIGHT_RED)    GPIOB->BSRR = GPIO_BSRR_BS4; else GPIOB->BSRR = GPIO_BSRR_BR4;
    if (lights_ns & LIGHT_YELLOW) GPIOB->BSRR = GPIO_BSRR_BS5; else GPIOB->BSRR = GPIO_BSRR_BR5;
    if (lights_ns & LIGHT_GREEN)  GPIOB->BSRR = GPIO_BSRR_BS6; else GPIOB->BSRR = GPIO_BSRR_BR6;
    if (lights_eo & LIGHT_RED)    GPIOB->BSRR = GPIO_BSRR_BS7; else GPIOB->BSRR = GPIO_BSRR_BR7;
    if (lights_eo & LIGHT_YELLOW) GPIOB->BSRR = GPIO_BSRR_BS8; else GPIOB->BSRR = GPIO_BSRR_BR8;
    if (lights_eo & LIGHT_GREEN)  GPIOB->BSRR = GPIO_BSRR_BS9; else GPIOB->BSRR = GPIO_BSRR_BR9;
    GPIOC->ODR ^= GPIO_ODR_ODR13;
}
