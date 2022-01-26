// Copyright 2017 jem@seethis
// Licensed under the MIT license (http://opensource.org/licenses/MIT)

#include "core/hardware.h"

#include <avr/io.h>

#include "avr_util.h"

#include "core/debug.h"
#include "core/error.h"
#include "core/io_map.h"
#include "core/nrf24.h"
#include "core/settings.h"

// add a little abstraction here so that we can move the SPI port around if desired
#define NRF24_SPI_PORT PORTC
#define NRF24_SPI SPIC

#ifndef NRF24_CE_PORT
#error "Need to define CE port"
#endif

#ifndef NRF24_CE_PIN
#error "Need to define CE pin"
#endif

#define SS   4
#define MOSI 5
#define MISO 6
#define SCK  7

#define SPI_PIN_MASK ((1<<SS) | (1<<MOSI) | (1<<MISO) | (1<<SCK))

#ifndef NRF24_IRQ_PORT
#define NRF24_IRQ_PORT         DEF_AVR_PORT(NRF24_IRQ_PIN_PORT)
#define NRF24_IRQ_PIN_MASK     DEF_AVR_PIN_MASK(NRF24_IRQ_PIN_NUM)
#define NRF24_IRQ_INT_VECT     DEF_AVR_INT_VECT(NRF24_IRQ_PIN_PORT, NRF24_IRQ_INT_NUM)
#define NRF24_IRQ_INTMASK      DEF_AVR_INT_INTMASK(NRF24_IRQ_PIN_PORT, NRF24_IRQ_INT_NUM)
#define NRF24_IRQ_INT_LVL      DEF_AVR_INT_LVL(NRF24_IRQ_INT_NUM, LO)
#define NRF24_IRQ_INT_LVL_MASK DEF_AVR_INT_LVL_MASK(NRF24_IRQ_INT_NUM)
#endif

#define MAX_NRF24_SPI_SPEED 10000000UL

// enable SPI master, mode 0, msb
#define SPI_MODE (SPI_ENABLE_bm | SPI_MASTER_bm | SPI_MODE_0_gc)

// NOTE: @32MHz, spi bit rate is 8Mbps. nRF24L01+ max SPI rate is 10Mbps
#if ((F_CPU/2) <= MAX_NRF24_SPI_SPEED)
#define SPI_SETTINGS (SPI_MODE | SPI_CLK2X_bm | SPI_PRESCALER_DIV4_gc)
#elif ((F_CPU/4) <= MAX_NRF24_SPI_SPEED)
#define SPI_SETTINGS (SPI_MODE | SPI_PRESCALER_DIV4_gc)
#else
#define SPI_SETTINGS (SPI_MODE | SPI_PRESCALER_DIV8_gc)
#endif

#if ((CLOCK_SPEED_SLOW/2) <= MAX_NRF24_SPI_SPEED)
#define SPI_SETTINGS_SLOW_CLOCK (SPI_SETTINGS | SPI_CLK2X_bm | SPI_PRESCALER_DIV4_gc)
#elif ((CLOCK_SPEED_SLOW/4) <= MAX_NRF24_SPI_SPEED)
#define SPI_SETTINGS_SLOW_CLOCK (SPI_SETTINGS | SPI_PRESCALER_DIV4_gc)
#else
#define SPI_SETTINGS_SLOW_CLOCK (SPI_SETTINGS | SPI_PRESCALER_DIV8_gc)
#endif

bool is_nrf24_initialized = false;

static void spi_init(PORT_t *port, SPI_t *spi) {
    port->DIRSET = (1<<SS) | (1<<MOSI) | (1<<SCK); // outputs
    port->OUTSET = (1<<SS); // pull high so slave is inactive

    // spi is polling based
    spi->INTCTRL = 0x00;

    if (g_slow_clock_mode) {
        spi->CTRL = SPI_SETTINGS_SLOW_CLOCK;
    } else {
        spi->CTRL = SPI_SETTINGS;
    }

    while (spi->STATUS & SPI_IF_bm) {
        *((volatile uint8_t*)&spi->DATA);
    }

}

inline static uint8_t spi_send_byte(SPI_t *spi, uint8_t byte) {
    spi->DATA = byte;
    // TODO/Note: use idle sleep mode here instead of busy loop to save power ???
    //
    // It might take longer to enter idle sleep mode than to receive a response
    // though. At mcu clock at 2MHz, then SPI is 1Mbps. To send a byte, it'll
    // take 8µs (16 cycles, really 12 since it takes 4 cycles to wake from
    // sleep), so if it takes longer that to enter sleep, then there's no
    // point.
    //
    // However, if we use prescaler of 4 for the SPI clock when running with
    // faster clock (USB mode CPU@32MHz and SPI@8Mbps), then idle sleep mode
    // will make a difference.
    while (!(spi->STATUS & SPI_IF_bm));
    return spi->DATA;
}

inline static void spi_ss(PORT_t *port, uint8_t val) {
    if (val) {
        port->OUTSET = (1<<SS); // release ss
    } else {
        port->OUTCLR = (1<<SS); // pull ss
    }
}

inline static void nrf24_ce_set(PORT_t *port, uint8_t val) {
    if (val) {
        port->OUTSET = NRF24_CE_PIN;
    } else {
        port->OUTCLR = NRF24_CE_PIN;
    }
}

inline static void nrf24_ce_init(PORT_t *port) {
    port->DIRSET = NRF24_CE_PIN;
}

uint8_t nrf24_spi_send_byte(uint8_t byte) {
    return spi_send_byte(&NRF24_SPI, byte);
}

void nrf24_csn(uint8_t val) {
    spi_ss(&NRF24_SPI_PORT, val);
}

void nrf24_ce(uint8_t val) {
    nrf24_ce_set(&NRF24_CE_PORT, val);
}

#define SPI_TIMEOUT_COUNTER 32000
/// Test if the SPI connection is working.
///
/// @return non-zero on error
static uint8_t nrf24_test_spi_connection(SPI_t *spi) {
    uint16_t counter = 0;
    spi->DATA = NRF_NOP;
    while (!(spi->STATUS & SPI_IF_bm)) {
        counter += 1;
        if (counter > SPI_TIMEOUT_COUNTER) {
            return -1;
        }
    }
    return 0;
}

void nrf24_init(void) {
    if (is_nrf24_initialized || g_runtime_settings.feature.ctrl.rf_disabled) {
        return;
    }

    is_nrf24_initialized = true;

    if (io_map_claim_pins(PORT_TO_NUM(NRF24_SPI_PORT), SPI_PIN_MASK)) {
        register_error(ERROR_PIN_MAPPING_CONFLICT);
        return;
    }

    spi_init(&NRF24_SPI_PORT, &NRF24_SPI);

    io_map_claim_pins(PORT_TO_NUM(NRF24_CE_PORT), NRF24_CE_PIN);
    io_map_claim_pins(PORT_TO_NUM(NRF24_IRQ_PORT), NRF24_IRQ_PIN_MASK);

    if (has_critical_error()) {
        register_error(ERROR_PIN_MAPPING_CONFLICT);
        return;
    }

    if (nrf24_test_spi_connection(&NRF24_SPI)) {
        register_error(ERROR_NRF24_BAD_SPI_CONNECTION);
        return;
    }

    nrf24_ce_init(&NRF24_CE_PORT);
    nrf24_ce_set(&NRF24_CE_PORT, 0);

}

void nrf24_disable(void) {
    if (is_nrf24_initialized) {
        nrf24_power_set(0);
        NRF24_SPI_PORT.DIRCLR = (1<<SS) | (1<<MOSI) | (1<<SCK); // inputs
        NRF24_CE_PORT.DIRSET = NRF24_CE_PIN;
        is_nrf24_initialized = false;
    }
}

#include <avr/interrupt.h>
#include "core/rf.h"

// setup irq pin interrupt
void rf_init_receive_irq(void) {
    // Pin as input
    NRF24_IRQ_PORT.DIRCLR = NRF24_IRQ_PIN_MASK;

    // Generate when IRQ pin is pulled low
    PORTCFG.MPCMASK = NRF24_IRQ_PIN_MASK;

    // NOTE: Can probably use the low level intterupt as well, but since the
    // internal pull-up's is too weak, the irq signal line tends to generate
    // two interrupts before the irq line is released.
    NRF24_IRQ_PORT.PIN0CTRL = PORT_OPC_PULLUP_gc | PORT_ISC_LEVEL_gc;
    // NRF24_IRQ_PORT.PIN0CTRL = PORT_OPC_PULLUP_gc | PORT_ISC_FALLING_gc;

    // Set interrupt priority
    NRF24_IRQ_PORT.INTCTRL =
        (NRF24_IRQ_PORT.INTCTRL & ~NRF24_IRQ_INT_LVL_MASK) | NRF24_IRQ_INT_LVL;

    // Enable the pin as an intterupt source on the corresponding ISR
    NRF24_IRQ_INTMASK |= NRF24_IRQ_PIN_MASK;

}

// Disable the interrupt
void rf_enable_receive_irq(void) {
    NRF24_IRQ_INTMASK |= NRF24_IRQ_PIN_MASK;
}

// Enable the interrupt
void rf_disable_receive_irq(void) {
    NRF24_IRQ_INTMASK &= ~NRF24_IRQ_PIN_MASK;
}


uint8_t nrf24_irq(void) {
    return NRF24_IRQ_PORT.IN & NRF24_IRQ_PIN_MASK;
}

ISR(NRF24_IRQ_INT_VECT) {
    debug_set(1, 0);
    rf_isr();
    debug_set(1, 1);
}
