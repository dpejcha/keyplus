// Copyright 2017 jem@seethis.link
// Licensed under the MIT license (http://opensource.org/licenses/MIT)
/// @file xmega/matrix_scanner.c
///
/// @brief Xmega default matrix scanner module

#include "core/matrix_scanner.h"

#include "config.h"

#include <string.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <util/delay_basic.h>

#include "core/error.h"
#include "core/hardware.h"
#include "core/io_map.h"
#include "core/timer.h"

#include "core/usb_commands.h"

#include "xmega_hardware.h"

// avr common code
#include "arch/avr/matrix_scanner.h"

// TODO: make these settings configurable from the settings module

static bool has_scan_irq_triggered;

#if !USE_HARDWARE_SPECIFIC_SCAN

static XRAM uint8_t s_col_masks[IO_PORT_COUNT];

static uint8_t s_row_port_masks[IO_PORT_COUNT];
static uint8_t s_row_pin_mask[MAX_NUM_ROWS];
static io_port_t *s_row_ports[MAX_NUM_ROWS];

static uint8_t s_bytes_per_row;

static uint8_t s_parasitic_discharge_delay_idle;
static uint8_t s_parasitic_discharge_delay_debouncing;

/// Setup the columns as inputs with pull ups
static void setup_columns(void) {
    // Note: DIR: 0 -> input, 1 -> output

    for (uint8_t col_pin_i=0; col_pin_i < g_scan_plan.cols; col_pin_i++) {
        const uint8_t pin_number = io_map_get_col_pin(col_pin_i);
        const uint8_t col_port_num = IO_MAP_GET_PIN_PORT(pin_number);
        const uint8_t col_pin_bit = IO_MAP_GET_PIN_BIT(pin_number);
        s_col_masks[col_port_num] |= (1 << col_pin_bit);
    }

    // Note: PORTCFG.MPCMASK lets us configure multiple PINnCTRL regs at once
    // It is cleared automatically after any PINnCTRL register is written
    // Note: If MPCMASK=0, then its function is disabled, so writing to PIN0CTRL
    // would actually affect update PIN0CTRL instead of updating no pins.
    const uint8_t max_col_pin_num = g_scan_plan.max_col_pin_num;
    const uint8_t max_port_num = INT_DIV_ROUND_UP(max_col_pin_num+1, IO_PORT_SIZE);
    uint8_t port_ii;
    for (port_ii = 0; port_ii < max_port_num; ++port_ii) {
        io_port_t *port = IO_MAP_GET_PORT(port_ii);
        uint8_t col_mask = s_col_masks[port_ii];

        // Nothing is set in this col
        if (col_mask == 0) {
            continue;
        }

        // Try to claim the pins
        if (io_map_claim_pins(port_ii, col_mask)) {
            register_error(ERROR_PIN_MAPPING_CONFLICT);
            return; // return on error
        }

        // Hardware setup for the pin
        //
        // Initialize the pins, as inputs with pull-up resistors and
        // enable the interrupts on both rising/falling edges
        {
            port->DIRCLR = col_mask;
            port->INT0MASK |= col_mask;
            PORTCFG.MPCMASK = col_mask;
            switch (g_scan_plan.mode) {
                // These modes need a pull-down resistor and non-inverted input
                case MATRIX_SCANNER_MODE_ROW_COL:
                case MATRIX_SCANNER_MODE_PIN_VCC: {
                    // Diodes are facing from row to column ( row -->|-- col)
                    // Or pins are connected GPIO --switch--> VCC
                    port->PIN0CTRL =
                        // non-inverted input
                        PORT_OPC_PULLDOWN_gc | // pull-down resistor
                        PORT_ISC_BOTHEDGES_gc;
                } break;

                // These cases need a pull-up resistor and inverted input
                case MATRIX_SCANNER_MODE_COL_ROW:
                case MATRIX_SCANNER_MODE_PIN_GND: {
                    // Diodes are facing from column to row ( col -->|-- row)
                    // Or pins are connected GPIO --switch--> GND
                    port->PIN0CTRL =
                        PORT_INVEN_bm | // invert input
                        PORT_OPC_PULLUP_gc |  // pull-up resistor
                        PORT_ISC_BOTHEDGES_gc;
                } break;

                default: {
                    register_error(ERROR_UNSUPPORTED_SCAN_MODE);
                    return;
                } break;
            }
        }
    }
}

/// Setup rows for matrix scaning
///
/// Rows will use the Wired-AND pin configuration in output mode. When an
/// output pin is in Wired-AND mode, writting 1 to the pin disconnects it and
/// writting 0 to the pin connects it to GND.
static void setup_rows(void) {
    // Note: DIR: 0 -> input, 1 -> output
    memset(s_row_port_masks, 0, IO_PORT_COUNT);

    for (uint8_t row_pin_i=0; row_pin_i < g_scan_plan.rows; row_pin_i++) {
        const uint8_t pin_number = io_map_get_row_pin(row_pin_i);
        const uint8_t row_port_num = IO_MAP_GET_PIN_PORT(pin_number);
        const uint8_t row_pin_bit = IO_MAP_GET_PIN_BIT(pin_number);

        io_port_t *const port = IO_MAP_GET_PORT(row_port_num);
        const uint8_t row_bit_mask = (1 << row_pin_bit);

        if (io_map_claim_pins(row_port_num, row_bit_mask)) {
            // failed to claim
            register_error(ERROR_PIN_MAPPING_CONFLICT);
            return;
        }

        s_row_port_masks[row_port_num] |= row_bit_mask;
        s_row_pin_mask[row_pin_i] = row_bit_mask;
        s_row_ports[row_pin_i] = port;

        // Hardware setup for the pin
        port->DIRSET = row_bit_mask; // output
        port->OUTSET = row_bit_mask;
        PORTCFG.MPCMASK = row_bit_mask;
        switch (g_scan_plan.mode) {
            // These modes need a pull-down resistor and non-inverted input
            case MATRIX_SCANNER_MODE_ROW_COL:
            case MATRIX_SCANNER_MODE_PIN_VCC: {
                // Inverted output
                // Disconnected when writting 0 to PORT
                // Note: WiredOR disconnects the pin when written to 0
                port->PIN0CTRL =
                    PORT_INVEN_bm | // inverted output
                    PORT_OPC_WIREDOR_gc;
            } break;

            // These cases need a pull-up resistor and inverted input
            case MATRIX_SCANNER_MODE_COL_ROW:
            case MATRIX_SCANNER_MODE_PIN_GND: {
                // Non-inverted output
                // Disconnected when writting 1 to PORT
                // Note: WiredAND disconnects the pin when written to 1
                port->PIN0CTRL = PORT_OPC_WIREDAND_gc;
            } break;

            default: {
                register_error(ERROR_UNSUPPORTED_SCAN_MODE);
                return;
            } break;
        }
    }

}

///  makes all rows floating inputs
static inline void unselect_all_rows(void) {
// #if XMEGA_SERIES == A OR B
    PORTA.OUTSET = s_row_port_masks[PORT_A_NUM];
    PORTB.OUTSET = s_row_port_masks[PORT_B_NUM];
    PORTC.OUTSET = s_row_port_masks[PORT_C_NUM];
    PORTD.OUTSET = s_row_port_masks[PORT_D_NUM];
    PORTE.OUTSET = s_row_port_masks[PORT_E_NUM];
#if IO_PORT_COUNT >= 7
    PORTF.OUTSET = s_row_port_masks[PORT_F_NUM];
#endif
    PORTR.OUTSET = s_row_port_masks[PORT_R_NUM];
}

/// make all rows output low
static inline void select_all_rows(void) {
    PORTA.OUTCLR = s_row_port_masks[PORT_A_NUM];
    PORTB.OUTCLR = s_row_port_masks[PORT_B_NUM];
    PORTC.OUTCLR = s_row_port_masks[PORT_C_NUM];
    PORTD.OUTCLR = s_row_port_masks[PORT_D_NUM];
    PORTE.OUTCLR = s_row_port_masks[PORT_E_NUM];
#if IO_PORT_COUNT >= 7
    PORTF.OUTCLR = s_row_port_masks[PORT_F_NUM];
#endif
    PORTR.OUTCLR = s_row_port_masks[PORT_R_NUM];
}

/// When `select_all_rows()` has been called, this function can be used to
/// check if any key is down in any row.
bool matrix_has_active_row(void) {
    return (PORTA.IN & s_col_masks[PORT_A_NUM]) ||
           (PORTB.IN & s_col_masks[PORT_B_NUM]) ||
           (PORTC.IN & s_col_masks[PORT_C_NUM]) ||
           (PORTD.IN & s_col_masks[PORT_D_NUM]) ||
           (PORTE.IN & s_col_masks[PORT_E_NUM]) ||
#if IO_PORT_COUNT >= 7
           (PORTF.IN & s_col_masks[PORT_F_NUM]) ||
#endif
           (PORTR.IN & s_col_masks[PORT_R_NUM]);
}

port_mask_t get_col_mask(uint8_t port_num) {
    return s_col_masks[port_num];
}

/// Selecting a row makes it outputs
static inline void select_row(uint8_t row) {
    io_port_t *port = s_row_ports[row];
    const uint8_t mask = s_row_pin_mask[row];
    port->OUTCLR = mask;
}

// unselecting a row disconnects it
static inline void unselect_row(uint8_t row) {
    io_port_t *const port = s_row_ports[row];
    const uint8_t mask = s_row_pin_mask[row];
    port->OUTSET = mask;
}

void matrix_scanner_init(void) {
    if (
        // g_scan_plan.cols > MAX_NUM_COLS ||
        g_scan_plan.rows > MAX_NUM_ROWS ||
        g_scan_plan.max_col_pin_num > IO_PORT_MAX_PIN_NUM
    ) {
        memset((uint8_t*)&g_scan_plan, 0, sizeof(matrix_scan_plan_t));
        register_error(ERROR_MATRIX_PINS_CONFIG_TOO_LARGE);
        return;
    }


    s_bytes_per_row = INT_DIV_ROUND_UP(g_scan_plan.max_col_pin_num+1, IO_PORT_SIZE);

    if (
        g_scan_plan.mode == MATRIX_SCANNER_MODE_COL_ROW ||
        g_scan_plan.mode == MATRIX_SCANNER_MODE_ROW_COL
    ) {
        setup_rows();
    }
    setup_columns();

    // set the rows and columns to their inital state.
    matrix_scan_irq_disable();
    unselect_all_rows();

    init_matrix_scanner_utils();

    // Configure the parasitic discharge delay based on how fast the mcu clock
    // is running
    if (g_slow_clock_mode) {
        const uint8_t base_factor = (16000000/1000000);
        const uint8_t slow_factor = (CLOCK_SPEED_SLOW/1000000);
        s_parasitic_discharge_delay_idle =
            (((uint16_t)g_scan_plan.parasitic_discharge_delay_idle * slow_factor) / base_factor);
        s_parasitic_discharge_delay_debouncing =
            (((uint16_t)g_scan_plan.parasitic_discharge_delay_debouncing * slow_factor) /base_factor);
    } else {
        s_parasitic_discharge_delay_idle = g_scan_plan.parasitic_discharge_delay_idle;
        s_parasitic_discharge_delay_debouncing = g_scan_plan.parasitic_discharge_delay_debouncing;
    }
}

static void matrix_scan_irq_clear_flags(void) {
    PORTA.INTFLAGS |= PORT_INT0IF_bm; // clear the interrupt flags
    PORTB.INTFLAGS |= PORT_INT0IF_bm;
    PORTC.INTFLAGS |= PORT_INT0IF_bm;
    PORTD.INTFLAGS |= PORT_INT0IF_bm;
    PORTE.INTFLAGS |= PORT_INT0IF_bm;
#if IO_PORT_COUNT >= 7
    PORTF.INTFLAGS |= PORT_INT0IF_bm;
#endif
    PORTR.INTFLAGS |= PORT_INT0IF_bm;
}

bool matrix_scan_irq_has_triggered(void) {
    return has_scan_irq_triggered;
}

void matrix_scan_irq_clear(void) {
    has_scan_irq_triggered = 0;
}

// Pins are set like this while scanner is initialized:
// columns: input pull-up
// rows: output-low
// This fuction, then enables intterupts on the
// So when any key is pressed, the columns will be driven low. We use this
// to generate interrupts on the column pins that will be triggered when
// any key is pressed.
void matrix_scan_irq_enable(void) {
    select_all_rows();

    PARASITIC_DISCHARGE_DELAY_SLOW_CLOCK(
        s_parasitic_discharge_delay_idle
    );

    matrix_scan_irq_clear_flags();
    matrix_scan_irq_clear();
    PORTA.INTCTRL = (PORTA.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_LO_gc;
    PORTB.INTCTRL = (PORTB.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_LO_gc;
    PORTC.INTCTRL = (PORTC.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_LO_gc;
    PORTD.INTCTRL = (PORTD.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_LO_gc;
    PORTE.INTCTRL = (PORTE.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_LO_gc;
#if IO_PORT_COUNT >= 7
    PORTF.INTCTRL = (PORTF.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_LO_gc;
#endif
    PORTR.INTCTRL = (PORTR.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_LO_gc;
}

void matrix_scan_irq_disable(void) {
    PORTA.INTCTRL = (PORTA.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_OFF_gc;
    PORTB.INTCTRL = (PORTB.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_OFF_gc;
    PORTC.INTCTRL = (PORTC.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_OFF_gc;
    PORTD.INTCTRL = (PORTD.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_OFF_gc;
    PORTE.INTCTRL = (PORTE.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_OFF_gc;
#if IO_PORT_COUNT >= 7
    PORTF.INTCTRL = (PORTF.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_OFF_gc;
#endif
    PORTR.INTCTRL = (PORTR.INTCTRL & ~PORT_INT0LVL_gm) | PORT_INT0LVL_OFF_gc;
    unselect_all_rows();
}

static void matrix_scan_irq(void) {
    matrix_scan_irq_clear_flags();
    has_scan_irq_triggered = 1;
}

ISR(PORTA_INT0_vect) { matrix_scan_irq(); }
ISR(PORTB_INT0_vect) { matrix_scan_irq(); }
ISR(PORTC_INT0_vect) { matrix_scan_irq(); }
ISR(PORTD_INT0_vect) { matrix_scan_irq(); }
ISR(PORTE_INT0_vect) { matrix_scan_irq(); }
ISR(PORTR_INT0_vect) { matrix_scan_irq(); }

static inline uint8_t scan_row(uint8_t row) {
    const uint8_t new_row[IO_PORT_COUNT] = {
        PORTA.IN & s_col_masks[PORT_A_NUM],
        PORTB.IN & s_col_masks[PORT_B_NUM],
        PORTC.IN & s_col_masks[PORT_C_NUM],
        PORTD.IN & s_col_masks[PORT_D_NUM],
        PORTE.IN & s_col_masks[PORT_E_NUM],
#if IO_PORT_COUNT >= 7
        PORTF.IN & s_col_masks[PORT_F_NUM],
#endif
        PORTR.IN & s_col_masks[PORT_R_NUM],
    };

    return scanner_debounce_row(row, new_row, s_bytes_per_row);
}

static inline bool matrix_scan_row_col_mode(void) {
    uint8_t row;
    bool scan_changed = false;

    for (row = 0; row < g_scan_plan.rows; ++row) {
        select_row(row);

        // After pulling a row low, an input column does not change
        // instantly because of parasitic capacitance.
        //
        // Parasitc capacitance of:
        // Xmega IO pin: 10pF
        // 1n4818 diode: 4pF
        // Cherry MX switch: 2pF
        //
        // The pull-up resistors in the xmega are 24kΩ. So we have a RC
        // circuit in the scanning matrix.
        // The RC circuit will take t=2RC to overcome the parasitic capacitance
        // and reach a low voltage (i.e. V < 0.3Vcc)
        //
        // MX switch and diodes are in series, so assume their combined
        // capacitance is min(4pF, 2pF) = 2pF.
        //
        // All the switch/diode pairs are connected in parallel with the input
        // pin on each row, so
        //
        // t = 2RC = 2 * 24_000 * (10 + 2*16) * 1e-12 == 2.016 µs
        // TODO: check if more error margin is needed
        // TODO: make this variable based on number of columns
        if (get_matrix_num_keys_debouncing()) {
            // Debouncing
            if (g_slow_clock_mode) {
                PARASITIC_DISCHARGE_DELAY_SLOW_CLOCK(
                    s_parasitic_discharge_delay_debouncing
                );
            } else {
                PARASITIC_DISCHARGE_DELAY_FAST_CLOCK(
                    s_parasitic_discharge_delay_debouncing
                );
            }
        } else {
            // not debouncing
            if (g_slow_clock_mode) {
                PARASITIC_DISCHARGE_DELAY_SLOW_CLOCK(
                    s_parasitic_discharge_delay_idle
                );
            } else {
                PARASITIC_DISCHARGE_DELAY_FAST_CLOCK(
                    s_parasitic_discharge_delay_idle
                );
            }
        }

        scan_changed |= scan_row(row);
        unselect_row(row);
    }

    return scan_changed;
}

static inline bool matrix_scan_pin_mode(void) {
    return scan_row(0);
}

bool matrix_scan(void) {
    switch (g_scan_plan.mode) {
        case MATRIX_SCANNER_MODE_COL_ROW:
        case MATRIX_SCANNER_MODE_ROW_COL: {
            return matrix_scan_row_col_mode();
        }

        case MATRIX_SCANNER_MODE_PIN_GND:
        case MATRIX_SCANNER_MODE_PIN_VCC: {
            return matrix_scan_pin_mode();
        }
    }
    return false;
}

#endif

/*********************************************************************
 *           Hardware specific implementation for scanner            *
 *********************************************************************/

#if USE_HARDWARE_SPECIFIC_SCAN

// TODO: move this to a separate file in the boards directory

#endif
