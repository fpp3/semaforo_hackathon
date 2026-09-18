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
    REG_TIME_INT7,       // Int 7: NS Amarillo, EO Amarillo         (Default:  0 s)
    REG_TIME_INT8,       // Int 8: NS Off, EO Off                   (Default:  0 s)

    /* --- 2. Estado de la FSM y Lámparas (Actualizado por vStateMachineTask) --- */
    REG_FSM_STATE,       // Estado actual de la FSM (0 a 7)
    REG_LIGHTS_NS,       // Luces Norte-Sur:  [Bit 0: R, Bit 1: A, Bit 2: V, Bit 3: Peatonal]
    REG_LIGHTS_EO,       // Luces Este-Oeste: [Bit 0: R, Bit 1: A, Bit 2: V, Bit 3: Peatonal]

    /* --- 3. Mediciones y Entradas (Actualizado por vReadInputsTask) ---------- */
    REG_BATTERY_MV,      // Tensión de batería en milivoltios (ej. 12400 = 12.4 V)
    REG_CURRENT_MA,      // Corriente total medida en mA
    REG_INPUTS_DIGITAL,  // Entradas digitales: [Bit 0: Red 220V presente, Bit 1: Noche LDR detectada]
    REG_FAULT_FLAGS,     // Banderas de fallas (Bit 0: Falla Lampara, Bit 1: Bateria baja)

    /* --- 4. Sensor LDR Crepuscular y Control de Modo ------------------------- */
    REG_LDR_MV,          // Tensión leída en el LDR (PB0 / ADC1 Ch 8) en mV (0 a 3300 mV)
    REG_LDR_THRESHOLD_MV,// Umbral de conmutación Noche en mV (Default: 2000 mV)
    REG_OP_MODE,         // Modo de operación: 0=Normal, 1=Destellante, 2=Todo Rojo, 3=Auto LDR

    /* --- 5. Umbrales de Corriente y Diagnóstico de Falla de Lámparas -------- */
    REG_LAMP_CURRENT_MA, // Corriente típica por lámpara activa en mA (Default: 200 mA)
    REG_CURRENT_TOL_PCT, // Margen de tolerancia de corriente en % (Default: 35%)
    REG_EXPECTED_CURR_MA,// Corriente calculada esperada según lámparas activas en mA

    REG_TOTAL_COUNT      // Total de registros asignados
} ModbusRegIndex_t;

/* Máscaras de bits para el estado de las lámparas */
#define LIGHT_RED        (1 << 0)
#define LIGHT_YELLOW     (1 << 1)
#define LIGHT_GREEN      (1 << 2)
#define LIGHT_ARROW_PED  (1 << 3)   // Flecha peatonal de paso (encendida/apagada)
#define LIGHT_PEDESTRIAN LIGHT_ARROW_PED // Alias para compatibilidad

/* Máscaras de bits para entradas digitales */
#define INPUT_MAINS_220V (1 << 0)   // Presencia de red eléctrica 220V AC
#define INPUT_LDR_NIGHT  (1 << 1)   // Condición de noche (tensión LDR >= umbral)

/* Máscaras de bits para banderas de falla (REG_FAULT_FLAGS) */
#define FAULT_LAMP_BURNED   (1 << 0) // Lámpara quemada / circuito abierto (subcorriente)
#define FAULT_BATTERY_LOW   (1 << 1) // Batería baja (<11.0V)
#define FAULT_OVERCURRENT   (1 << 2) // Sobrecorriente / cortocircuito

/* Modos de Operación (REG_OP_MODE) */
#define OP_MODE_NORMAL      0   // Ciclo normal FSM
#define OP_MODE_DESTELLANTE 1   // Destellante amarillo forzado
#define OP_MODE_TODO_ROJO   2   // Todo rojo forzado
#define OP_MODE_AUTO_LDR    3   // Automático: Conmuta a destellante si LDR detecta noche

/* Vector global compartido entre tareas */
extern volatile uint16_t g_modbus_registers[REG_TOTAL_COUNT];

#endif /* MODBUS_REGISTERS_H */

