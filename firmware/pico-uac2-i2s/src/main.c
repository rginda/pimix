#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "quirk_os_guessing.h"

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "pico/unique_id.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "hardware/uart.h"
#include "i2s_slave.pio.h"

//--------------------------------------------------------------------+
// Pin Definitions (Bottom-left 5 pins of Pico header: Pins 16, 17, 18, 19, 20)
//   Pin 16 = GP12 (UART0 RX from Pi 5 via ISO7741 Ch C)
//   Pin 17 = GP13 (SDATA PIO Out -> Pi 5 GPIO 20/22/24/26 via ISO7741 Ch D)
//   Pin 18 = GND
//   Pin 19 = GP14 (BCLK PIO In 0 <- Pi 5 GPIO 18 via ISO7741 Ch A)
//   Pin 20 = GP15 (LRCLK PIO In 1 <- Pi 5 GPIO 19 via ISO7741 Ch B)
//--------------------------------------------------------------------+
#define PIN_UART_CTRL_RX    12
#define PIN_I2S_SDATA       13
#define PIN_I2S_BCLK_BASE   14

//--------------------------------------------------------------------+
// Audio Constants & Buffers
//--------------------------------------------------------------------+
#define SAMPLE_RATE_HZ      48000
#define I2S_DMA_FRAMES      48  // 1 ms of stereo audio at 48 kHz
#define I2S_DMA_WORDS       (I2S_DMA_FRAMES * 2) // 96 x 32-bit words (L, R interleaved)
#define USB_FRAME_BYTES     (I2S_DMA_FRAMES * CFG_TUD_AUDIO_FUNC_1_N_BYTES_PER_SAMPLE_RX * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX)

const uint32_t sample_rates[] = { SAMPLE_RATE_HZ };
uint32_t current_sample_rate  = SAMPLE_RATE_HZ;
#define N_SAMPLE_RATES TU_ARRAY_SIZE(sample_rates)

// UAC2 Volume range: -60.0 dB (-15360) to 0.0 dB (0) in 0.5 dB (128) steps
enum {
    VOLUME_CTRL_0_DB  = 0,
    VOLUME_CTRL_60_DB = 15360,
    VOLUME_STEP_HALF_DB = 128,
};

// Audio Feature Unit state (+1 for Master channel 0)
int8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1] = {0};
int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1] = {0};

// I2S Subcode state packed into bits [7:0] of Left and Right 32-bit I2S words:
//   Left[7:0]  = 0xA0 | (1 << 3) | ((eff_mute & 1) << 2) | (host_vol_seq & 0x03)
//   Right[7:0] = vol_half_db_step (0 = -60.0 dB, 120 = 0.0 dB, 144 = +12.0 dB)
static volatile uint8_t host_vol_seq = 0;
static volatile uint8_t subcode_vol_step = 120; // Default 0.0 dB (120 * 0.5 - 60.0 = 0.0)
static volatile bool pending_int_vol = false;
static volatile bool pending_int_mute = false;
static uint8_t my_pico_index = 0; // 0 = USB 1, 1 = USB 2, 2 = USB 3, 3 = USB 4

// Ping-pong DMA buffers feeding PIO0 TX FIFO (interleaved Left, Right 32-bit MSB-aligned words)
static uint32_t __attribute__((aligned(4))) i2s_dma_buf_a[I2S_DMA_WORDS];
static uint32_t __attribute__((aligned(4))) i2s_dma_buf_b[I2S_DMA_WORDS];

static int dma_ch_a = -1;
static int dma_ch_b = -1;

// Counters incremented by DMA ISR each time a 1ms (48-frame) buffer finishes transmitting over I2S
static volatile uint32_t dma_buf_a_needs_fill = 0;
static volatile uint32_t dma_buf_b_needs_fill = 0;
static volatile uint32_t dma_total_completions = 0;

// Temporary unpack buffer for USB S24_3LE packets
static uint8_t usb_raw_bytes[USB_FRAME_BYTES];

// Built-in test tone mode (0 = off / normal USB audio; >0 = frequency in Hz)
static volatile uint16_t test_tone_freq_hz = 0;
static uint32_t test_tone_phase = 0;

//--------------------------------------------------------------------+
// Helper: Sync UAC2 volume/mute <-> I2S Subcode & UAC2 Interrupt EP
//--------------------------------------------------------------------+
static void update_subcode_from_host_uac2(void) {
    // Convert host UAC2 volume[0] (-15360 .. 0) to half-dB steps above -60.0 dB (0 .. 120)
    int32_t v = volume[0];
    if (v < -VOLUME_CTRL_60_DB) v = -VOLUME_CTRL_60_DB;
    if (v > VOLUME_CTRL_0_DB) v = VOLUME_CTRL_0_DB;
    int32_t step = (v + VOLUME_CTRL_60_DB + 64) / VOLUME_STEP_HALF_DB;
    if (step < 0) step = 0;
    if (step > 120) step = 120;
    subcode_vol_step = (uint8_t)step;
    host_vol_seq = (host_vol_seq + 1) & 0x03;
}

static void apply_remote_fader_from_pi5(uint8_t new_mute, uint8_t new_vol_step) {
    if (new_vol_step > 144) new_vol_step = 144;
    subcode_vol_step = new_vol_step;

    int8_t m = new_mute ? 1 : 0;
    if (mute[0] != m) {
        mute[0] = m;
        mute[1] = m;
        mute[2] = m;
        pending_int_mute = true;
    }

    // Clamp > 0 dB UI fader boost to 0 dB on the host OS slider
    int32_t clamped_step = (new_vol_step > 120) ? 120 : (int32_t)new_vol_step;
    int16_t new_uac_vol = (int16_t)((clamped_step * VOLUME_STEP_HALF_DB) - VOLUME_CTRL_60_DB);
    if (volume[0] != new_uac_vol) {
        volume[0] = new_uac_vol;
        volume[1] = new_uac_vol;
        volume[2] = new_uac_vol;
        pending_int_vol = true;
    }
}

static void service_uac2_interrupt_ep(void) {
    if (!tud_audio_mounted()) return;

    if (pending_int_mute) {
        audio_interrupt_data_t int_msg = {
            .bInfo = 0,
            .bAttribute = AUDIO_CS_REQ_CUR,
            .wValue_cn_or_mcn = 0,
            .wValue_cs = AUDIO_FU_CTRL_MUTE,
            .wIndex_ep_or_int = ITF_NUM_AUDIO_CONTROL,
            .wIndex_entity_id = UAC2_ENTITY_FEATURE_UNIT,
        };
        if (tud_audio_int_write(&int_msg)) {
            pending_int_mute = false;
            return;
        }
    }

    if (pending_int_vol) {
        audio_interrupt_data_t int_msg = {
            .bInfo = 0,
            .bAttribute = AUDIO_CS_REQ_CUR,
            .wValue_cn_or_mcn = 0,
            .wValue_cs = AUDIO_FU_CTRL_VOLUME,
            .wIndex_ep_or_int = ITF_NUM_AUDIO_CONTROL,
            .wIndex_entity_id = UAC2_ENTITY_FEATURE_UNIT,
        };
        if (tud_audio_int_write(&int_msg)) {
            pending_int_vol = false;
        }
    }
}

//--------------------------------------------------------------------+
// UART0 RX Control Listener on GP12 (115200 baud, 4-byte frame)
//   Frame: [0xA5, pico_idx (0..3), (mute << 7) | (vol_step & 0x7F), checksum]
//--------------------------------------------------------------------+
static void uart_ctrl_init(void) {
    char uid_str[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    pico_get_unique_board_id_string(uid_str, sizeof(uid_str));
    if (strcmp(uid_str, "E66164084335BC2E") == 0) {
        my_pico_index = 0; // PiMix USB 1
    } else if (strcmp(uid_str, "E66164084354A62E") == 0) {
        my_pico_index = 1; // PiMix USB 2
    }

    uart_init(uart0, 115200);
    gpio_set_function(PIN_UART_CTRL_RX, GPIO_FUNC_UART);
    gpio_pull_up(PIN_UART_CTRL_RX);
}

static void uart_ctrl_task(void) {
    static uint8_t pkt[4];
    static uint8_t idx = 0;

    while (uart_is_readable(uart0)) {
        uint8_t b = uart_getc(uart0);
        if (idx == 0) {
            if (b == 0xA5) pkt[idx++] = b;
        } else {
            pkt[idx++] = b;
            if (idx == 4) {
                idx = 0;
                uint8_t csum = (uint8_t)(pkt[0] ^ pkt[1] ^ pkt[2]);
                if (pkt[3] == csum && (pkt[1] & 0x0F) == my_pico_index) {
                    uint8_t m = (pkt[1] >> 4) & 1;
                    uint8_t vstep = pkt[2];
                    apply_remote_fader_from_pi5(m, vstep);
                }
            }
        }
    }
}

//--------------------------------------------------------------------+
// DMA + PIO I2S Slave Initialization & Interrupt Handler
//--------------------------------------------------------------------+
static void __isr dma_i2s_irq_handler(void) {
    if (dma_hw->ints0 & (1u << dma_ch_a)) {
        dma_hw->ints0 = (1u << dma_ch_a);
        dma_channel_set_read_addr(dma_ch_a, i2s_dma_buf_a, false);
        dma_channel_set_trans_count(dma_ch_a, I2S_DMA_WORDS, false);
        dma_buf_a_needs_fill++;
        dma_total_completions++;
    }
    if (dma_hw->ints0 & (1u << dma_ch_b)) {
        dma_hw->ints0 = (1u << dma_ch_b);
        dma_channel_set_read_addr(dma_ch_b, i2s_dma_buf_b, false);
        dma_channel_set_trans_count(dma_ch_b, I2S_DMA_WORDS, false);
        dma_buf_b_needs_fill++;
        dma_total_completions++;
    }
}

static void i2s_slave_init(void) {
    memset(i2s_dma_buf_a, 0, sizeof(i2s_dma_buf_a));
    memset(i2s_dma_buf_b, 0, sizeof(i2s_dma_buf_b));

    PIO pio = pio0;
    uint sm = 0;
    uint offset = pio_add_program(pio, &i2s_slave_tx_program);
    i2s_slave_tx_program_init(pio, sm, offset, PIN_I2S_SDATA, PIN_I2S_BCLK_BASE);

    dma_ch_a = dma_claim_unused_channel(true);
    dma_ch_b = dma_claim_unused_channel(true);

    dma_channel_config cfg_a = dma_channel_get_default_config(dma_ch_a);
    channel_config_set_transfer_data_size(&cfg_a, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_a, true);
    channel_config_set_write_increment(&cfg_a, false);
    channel_config_set_dreq(&cfg_a, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&cfg_a, dma_ch_b);

    dma_channel_configure(
        dma_ch_a,
        &cfg_a,
        &pio->txf[sm],
        i2s_dma_buf_a,
        I2S_DMA_WORDS,
        false
    );

    dma_channel_config cfg_b = dma_channel_get_default_config(dma_ch_b);
    channel_config_set_transfer_data_size(&cfg_b, DMA_SIZE_32);
    channel_config_set_read_increment(&cfg_b, true);
    channel_config_set_write_increment(&cfg_b, false);
    channel_config_set_dreq(&cfg_b, pio_get_dreq(pio, sm, true));
    channel_config_set_chain_to(&cfg_b, dma_ch_a);

    dma_channel_configure(
        dma_ch_b,
        &cfg_b,
        &pio->txf[sm],
        i2s_dma_buf_b,
        I2S_DMA_WORDS,
        false
    );

    dma_channel_set_irq0_enabled(dma_ch_a, true);
    dma_channel_set_irq0_enabled(dma_ch_b, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_i2s_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    dma_channel_start(dma_ch_a);
}

//--------------------------------------------------------------------+
// Unpack 24-bit USB audio (S24_3LE) into 32-bit MSB-aligned I2S buffer
// while embedding 8-bit Host Volume/Mute Subcode in bits [7:0]
//--------------------------------------------------------------------+
static void fill_i2s_buffer_from_usb(uint32_t *dst_words) {
    uint16_t bytes_read = tud_audio_read(usb_raw_bytes, USB_FRAME_BYTES);

    bool is_muted = mute[0] || (mute[1] && mute[2]);
    uint8_t sub_l = (uint8_t)(0xA8u | ((is_muted ? 1u : 0u) << 2) | (host_vol_seq & 0x03u));
    uint8_t sub_r = subcode_vol_step;

    if (test_tone_freq_hz > 0) {
        uint32_t phase_inc = (uint32_t)(((uint64_t)test_tone_freq_hz << 32) / SAMPLE_RATE_HZ);
        for (uint32_t i = 0; i < I2S_DMA_FRAMES; i++) {
            test_tone_phase += phase_inc;
            uint32_t folded = (test_tone_phase & 0x80000000u) ? ~test_tone_phase : test_tone_phase;
            int32_t tri = (int32_t)((folded << 1) - 0x80000000u);
            int32_t sample = (tri >> 2) & 0xFFFFFF00;
            dst_words[i * 2 + 0] = ((uint32_t)sample & 0xFFFFFF00u) | sub_l;
            dst_words[i * 2 + 1] = ((uint32_t)(-sample) & 0xFFFFFF00u) | sub_r;
        }
        return;
    }

    if (bytes_read == 0) {
        for (uint32_t i = 0; i < I2S_DMA_FRAMES; i++) {
            dst_words[i * 2 + 0] = (uint32_t)sub_l;
            dst_words[i * 2 + 1] = (uint32_t)sub_r;
        }
        return;
    }

    uint32_t frames_received = bytes_read / 6;
    const uint8_t *src = usb_raw_bytes;

    for (uint32_t i = 0; i < frames_received && i < I2S_DMA_FRAMES; i++) {
        // Pass unattenuated 24-bit PCM in bits [31:8]; PiMix CamillaDSP fader applies the volume
        uint32_t left = ((uint32_t)src[0] << 8) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 24);
        uint32_t right = ((uint32_t)src[3] << 8) | ((uint32_t)src[4] << 16) | ((uint32_t)src[5] << 24);

        dst_words[i * 2 + 0] = left | sub_l;
        dst_words[i * 2 + 1] = right | sub_r;
        src += 6;
    }

    for (uint32_t i = frames_received; i < I2S_DMA_FRAMES; i++) {
        dst_words[i * 2 + 0] = (uint32_t)sub_l;
        dst_words[i * 2 + 1] = (uint32_t)sub_r;
    }
}

static void audio_task(void) {
    static uint32_t last_a_filled = 0;
    static uint32_t last_b_filled = 0;
    static uint32_t last_dma_activity_ms = 0;
    static uint32_t fallback_ms = 0;

    uint32_t now_ms = board_millis();

    uint32_t cur_a = dma_buf_a_needs_fill;
    if (cur_a != last_a_filled) {
        last_a_filled = cur_a;
        last_dma_activity_ms = now_ms;
        fill_i2s_buffer_from_usb(i2s_dma_buf_a);
    }

    uint32_t cur_b = dma_buf_b_needs_fill;
    if (cur_b != last_b_filled) {
        last_b_filled = cur_b;
        last_dma_activity_ms = now_ms;
        fill_i2s_buffer_from_usb(i2s_dma_buf_b);
    }

    if ((now_ms - last_dma_activity_ms) > 2) {
        if (now_ms != fallback_ms) {
            fallback_ms = now_ms;
            fill_i2s_buffer_from_usb(i2s_dma_buf_a);
        }
    }

    service_uac2_interrupt_ep();
}

//--------------------------------------------------------------------+
// Main Loop
//--------------------------------------------------------------------+
int main(void) {
    board_init();
    uart_ctrl_init();
    i2s_slave_init();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    watchdog_enable(2000, 1);

    while (1) {
        watchdog_update();
        tud_task();
        uart_ctrl_task();
        audio_task();
    }
}

//--------------------------------------------------------------------+
// Vendor Control Request Callback (Remote BOOTSEL, Test Tone, Volume Sync & Diagnostics)
//--------------------------------------------------------------------+
typedef struct __attribute__((packed)) {
    uint32_t gpio_in_now;
    uint32_t gpio_or;
    uint32_t gpio_and;
    uint32_t dma_completions;
    uint32_t dma_a_rem;
    uint32_t dma_b_rem;
    uint32_t pio_pc_fstat;
    uint32_t buf_a0;
} pimix_diag_t;

static pimix_diag_t diag_resp;

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *request) {
    if (stage != CONTROL_STAGE_SETUP) return true;

    if (request->bmRequestType_bit.type == TUSB_REQ_TYPE_VENDOR) {
        if (request->bRequest == PIMIX_VENDOR_REQ_BOOTSEL && request->wValue == 0xB007) {
            reset_usb_boot(0, 0);
            return true;
        }
        if (request->bRequest == PIMIX_VENDOR_REQ_TEST_TONE) {
            test_tone_freq_hz = request->wValue;
            return tud_control_status(rhport, request);
        }
        if (request->bRequest == PIMIX_VENDOR_REQ_SET_VOL) {
            // wValue: high byte = mute (0 or 1), low byte = vol_half_db_step (0..144)
            uint8_t new_mute = (request->wValue >> 8) & 0x01;
            uint8_t new_step = request->wValue & 0xFF;
            apply_remote_fader_from_pi5(new_mute, new_step);
            return tud_control_status(rhport, request);
        }
        if (request->bRequest == PIMIX_VENDOR_REQ_FORCE_SDATA) {
            if (request->wValue == 0) {
                pio_gpio_init(pio0, PIN_I2S_SDATA);
                pio_sm_set_consecutive_pindirs(pio0, 0, PIN_I2S_SDATA, 1, true);
            } else {
                gpio_init(PIN_I2S_SDATA);
                gpio_set_dir(PIN_I2S_SDATA, GPIO_OUT);
                gpio_put(PIN_I2S_SDATA, (request->wValue == 2) ? 1 : 0);
            }
            return tud_control_status(rhport, request);
        }
        if (request->bRequest == PIMIX_VENDOR_REQ_DIAG && request->bmRequestType_bit.direction == TUSB_DIR_IN) {
            uint32_t g_or = 0;
            uint32_t g_and = 0xFFFFFFFFu;
            for (int i = 0; i < 4000; i++) {
                uint32_t v = sio_hw->gpio_in;
                g_or |= v;
                g_and &= v;
            }
            diag_resp.gpio_in_now = sio_hw->gpio_in;
            diag_resp.gpio_or = g_or;
            diag_resp.gpio_and = g_and;
            diag_resp.dma_completions = dma_total_completions;
            diag_resp.dma_a_rem = dma_hw->ch[dma_ch_a].transfer_count;
            diag_resp.dma_b_rem = dma_hw->ch[dma_ch_b].transfer_count;
            diag_resp.pio_pc_fstat = ((uint32_t)pio0->sm[0].addr << 16) | (pio0->fstat & 0xFFFFu);
            diag_resp.buf_a0 = i2s_dma_buf_a[0];
            uint16_t len = request->wLength < sizeof(diag_resp) ? request->wLength : sizeof(diag_resp);
            return tud_control_xfer(rhport, request, &diag_resp, len);
        }
    }
    return false;
}

//--------------------------------------------------------------------+
// UAC2 Audio Control Callbacks
//--------------------------------------------------------------------+
static bool tud_audio_clock_get_request(uint8_t rhport, audio_control_request_t const *request) {
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_4_t curf = { (int32_t)tu_htole32(current_sample_rate) };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &curf, sizeof(curf));
        } else if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_4_n_t(N_SAMPLE_RATES) rangef = {
                .wNumSubRanges = tu_htole16(N_SAMPLE_RATES)
            };
            for (uint8_t i = 0; i < N_SAMPLE_RATES; i++) {
                rangef.subrange[i].bMin = (int32_t)sample_rates[i];
                rangef.subrange[i].bMax = (int32_t)sample_rates[i];
                rangef.subrange[i].bRes = 0;
            }
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &rangef, sizeof(rangef));
        }
    } else if (request->bControlSelector == AUDIO_CS_CTRL_CLK_VALID && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t cur_valid = { .bCur = 1 };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_valid, sizeof(cur_valid));
    }
    return false;
}

static bool tud_audio_clock_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf) {
    (void)rhport;
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_CLOCK);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_CS_CTRL_SAM_FREQ) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_4_t));
        current_sample_rate = (uint32_t)((audio_control_cur_4_t const *)buf)->bCur;
        return true;
    }
    return false;
}

static bool tud_audio_feature_unit_get_request(uint8_t rhport, audio_control_request_t const *request) {
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_FEATURE_UNIT);

    uint8_t ch = request->bChannelNumber;
    if (ch > CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX) ch = 0;

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t mute1 = { .bCur = mute[ch] };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
    } else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) range_vol = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = {
                    .bMin = tu_htole16(-VOLUME_CTRL_60_DB),
                    .bMax = tu_htole16(VOLUME_CTRL_0_DB),
                    .bRes = tu_htole16(VOLUME_STEP_HALF_DB)
                }
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
        } else if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t cur_vol = { .bCur = tu_htole16(volume[ch]) };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
        }
    }
    return false;
}

static bool tud_audio_feature_unit_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf) {
    (void)rhport;
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_FEATURE_UNIT);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    uint8_t ch = request->bChannelNumber;
    if (ch > CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX) ch = 0;

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
        mute[ch] = ((audio_control_cur_1_t const *)buf)->bCur;
        if (ch != 0) mute[0] = (mute[1] && mute[2]) ? 1 : 0;
        update_subcode_from_host_uac2();
        return true;
    } else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
        volume[ch] = ((audio_control_cur_2_t const *)buf)->bCur;
        if (ch != 0) volume[0] = volume[ch];
        update_subcode_from_host_uac2();
        return true;
    }
    return false;
}

bool tud_audio_get_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;
    if (request->bEntityID == UAC2_ENTITY_CLOCK)
        return tud_audio_clock_get_request(rhport, request);
    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT)
        return tud_audio_feature_unit_get_request(rhport, request);
    return false;
}

bool tud_audio_set_req_entity_cb(uint8_t rhport, tusb_control_request_t const *p_request, uint8_t *buf) {
    audio_control_request_t const *request = (audio_control_request_t const *)p_request;
    if (request->bEntityID == UAC2_ENTITY_FEATURE_UNIT)
        return tud_audio_feature_unit_set_request(rhport, request, buf);
    if (request->bEntityID == UAC2_ENTITY_CLOCK)
        return tud_audio_clock_set_request(rhport, request, buf);
    return false;
}

bool tud_audio_set_itf_close_EP_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    (void)p_request;
    return true;
}

bool tud_audio_set_itf_cb(uint8_t rhport, tusb_control_request_t const *p_request) {
    (void)rhport;
    (void)p_request;
    return true;
}

void tud_audio_feedback_params_cb(uint8_t func_id, uint8_t alt_itf, audio_feedback_params_t *feedback_param) {
    (void)func_id;
    (void)alt_itf;
    feedback_param->method = AUDIO_FEEDBACK_METHOD_FIFO_COUNT;
    feedback_param->sample_freq = current_sample_rate;
}

bool tud_audio_feedback_format_correction_cb(uint8_t func_id) {
    (void)func_id;
    if (tud_speed_get() == TUSB_SPEED_FULL && quirk_os_guessing_get() == QUIRK_OS_GUESSING_OSX) {
        return true;
    }
    return false;
}
