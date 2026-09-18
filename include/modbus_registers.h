#ifndef MODBUS_REGISTERS_H
#define MODBUS_REGISTERS_H

#include <stdint.h>

/* ========================================================================= */
/* MAPA DE REGISTROS MODBUS (Holding / Input Registers - 16 bits)            */
/* Compartido entre las 3 tareas de FreeRTOS                                 */
/* ========================================================================= */
typedef enum {
    /* --- 1. Intervalos de tiempo configurables por el Maestro (en segundos) --- */
    /* Valores por defecto basados en central_semaforo.pdf (Página 9)            */
    REG_TIME_INT1 = 0,   // Int 1: NS Verde (+ Peatonal), EO Rojo   (Default: 25 s)
    REG_TIME_INT2,       // Int 2: NS Amarillo, EO Rojo             (Default:  3 s)
    REG_TIME_INT3,       // Int 3: NS Rojo, EO Rojo (Despeje)       (Default:  2 s)
    REG_TIME_INT4,       // Int 4: NS Rojo, EO Verde (+ Peatonal)   (Default: 25 s)
    REG_TIME_INT5,       // Int 5: NS Rojo, EO Amarillo             (Default:  3 s)
    REG_TIME_INT6,       // Int 6: NS Rojo, EO Rojo (Despeje)       (Default:  2 s)

    /* --- 2. Estado de la FSM y Lámparas (Actualizado por vStateMachineTask) --- */
    REG_FSM_STATE,       // Estado actual de la FSM (0 a 5, o 99=Intermitente)
    REG_LIGHTS_NS,       // Luces Norte-Sur:  [Bit 0: R, Bit 1: A, Bit 2: V, Bit 3: Peatonal]
    REG_LIGHTS_EO,       // Luces Este-Oeste: [Bit 0: R, Bit 1: A, Bit 2: V, Bit 3: Peatonal]

    /* --- 3. Mediciones y Entradas (Actualizado por vReadInputsTask) ---------- */
    REG_BATTERY_MV,      // Tensión de batería en milivoltios (ej. 12400 = 12.4 V)
    REG_CURRENT_MA,      // Corriente total medida en mA
    REG_INPUTS_DIGITAL,  // Entradas digitales: [Bit 0: Red 220V, Bit 1: Boton NS, Bit 2: Boton EO]
    REG_FAULT_FLAGS,     // Banderas de fallas (Bit 0: Falla Lampara, Bit 1: Bateria baja)

    /* --- 4. Modo de Operación (Modificable por Maestro o Local) -------------- */
    REG_OP_MODE,         // 0: Normal (Ciclo FSM), 1: Amarillo Intermitente, 2: Todo Rojo

    REG_TOTAL_COUNT      // Total de registros asignados
} ModbusRegIndex_t;

/* Máscaras de bits para el estado de las lámparas */
#define LIGHT_RED        (1 << 0)
#define LIGHT_YELLOW     (1 << 1)
#define LIGHT_GREEN      (1 << 2)
#define LIGHT_PEDESTRIAN (1 << 3)

/* Máscaras de bits para entradas digitales */
#define INPUT_MAINS_220V (1 << 0)
#define INPUT_BUTTON_NS  (1 << 1)
#define INPUT_BUTTON_EO  (1 << 2)

/* Vector global compartido entre tareas */
extern volatile uint16_t g_modbus_registers[REG_TOTAL_COUNT];

#endif /* MODBUS_REGISTERS_H */

