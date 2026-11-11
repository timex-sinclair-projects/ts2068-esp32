// APS6404L PSRAM on QMI CS1.
//
// From Adafruit's RP2350 PSRAM setup (MIT), as used for this board by
// paulscottrobson/rp2350-modules (modules/psram/library/rp2350pc_psram.c).
// Unlike that version, the flash (M0) timing is left alone: the board's
// flash divider is set before the clock is raised (main.c), as on the
// TS-Pico v3.

#include "psram.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/sync.h"

static size_t __no_inline_not_in_flash_func(psram_detect)(void) {
    size_t size = 0;
    uint32_t irq = save_and_disable_interrupts();

    qmi_hw->direct_csr = 30 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
    // Wait out the cooldown of the last XIP transfer.
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }

    // Leave QPI mode, in case it was set up already (0xF5, sent quad).
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS | QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB | 0xf5;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }
    (void)qmi_hw->direct_rx;
    qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;

    // Read ID (0x9F): 3 address bytes, then KGD and EID.
    qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
    uint8_t kgd = 0, eid = 0;
    for (int i = 0; i < 7; i++) {
        qmi_hw->direct_tx = i == 0 ? 0x9f : 0xff;
        while ((qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS) == 0) {
        }
        while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
        }
        uint8_t v = (uint8_t)qmi_hw->direct_rx;
        if (i == 5) kgd = v;
        if (i == 6) eid = v;
    }
    qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);

    if (kgd == 0x5d) {
        size = 1024 * 1024;
        uint8_t size_id = eid >> 5;
        if (eid == 0x26 || size_id == 2) {
            size *= 8;
        } else if (size_id == 0) {
            size *= 2;
        } else if (size_id == 1) {
            size *= 4;
        }
    }

    restore_interrupts(irq);
    return size;
}

size_t __no_inline_not_in_flash_func(psram_init)(uint cs_pin) {
    gpio_set_function(cs_pin, GPIO_FUNC_XIP_CS1);

    size_t size = psram_detect();
    if (!size) return 0;

    // Timing for up to 133 MHz; at 252 MHz: divider 2 (126 MHz), RX delay 3.
    // Worked out before direct mode, since clock_get_hz() runs from flash.
    const int max_psram_freq = 133000000;
    const int clock_hz = (int)clock_get_hz(clk_sys);
    int divisor = (clock_hz + max_psram_freq - 1) / max_psram_freq;
    if (divisor == 1 && clock_hz > 100000000) divisor = 2;
    int rxdelay = divisor;
    if (clock_hz / divisor > 100000000) rxdelay += 1;

    // Max select <= 8 us (units of 64 clocks); min deselect >= 18 ns.
    const int clock_period_fs = (int)(1000000000000000ll / clock_hz);
    const int max_select = (125 * 1000000) / clock_period_fs;
    const int min_deselect = (18 * 1000000 + (clock_period_fs - 1)) / clock_period_fs - (divisor + 1) / 2;

    uint32_t irq = save_and_disable_interrupts();

    // Direct mode, CS1 automatic, divider 10: enter QPI mode (0x35).
    qmi_hw->direct_csr = 10 << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_AUTO_CS1N_BITS;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }
    qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35;
    while (qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS) {
    }

    qmi_hw->m[1].timing = 1 << QMI_M1_TIMING_COOLDOWN_LSB |
        QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
        max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
        min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
        rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
        divisor << QMI_M1_TIMING_CLKDIV_LSB;

    // Quad read 0xEB with 6 dummy cycles; quad write 0x38.
    qmi_hw->m[1].rfmt =
        QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
        QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_RFMT_ADDR_WIDTH_LSB |
        QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
        QMI_M0_RFMT_DATA_WIDTH_VALUE_Q << QMI_M0_RFMT_DATA_WIDTH_LSB |
        QMI_M0_RFMT_PREFIX_LEN_VALUE_8 << QMI_M0_RFMT_PREFIX_LEN_LSB |
        6 << QMI_M0_RFMT_DUMMY_LEN_LSB;
    qmi_hw->m[1].rcmd = 0xeb;

    qmi_hw->m[1].wfmt =
        QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
        QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q << QMI_M0_WFMT_ADDR_WIDTH_LSB |
        QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
        QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
        QMI_M0_WFMT_DATA_WIDTH_VALUE_Q << QMI_M0_WFMT_DATA_WIDTH_LSB |
        QMI_M0_WFMT_PREFIX_LEN_VALUE_8 << QMI_M0_WFMT_PREFIX_LEN_LSB;
    qmi_hw->m[1].wcmd = 0x38;

    qmi_hw->direct_csr = 0;
    hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);

    restore_interrupts(irq);
    return size;
}
