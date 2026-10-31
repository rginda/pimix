#include <stdio.h>
#include <string.h>

#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "quirk_os_guessing.h"

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/watchdog.h"
#include "i2s_slave.pio.h"

//--------------------------------------------------------------------+
// Pin Definitions (Bottom-left 4 pins of Pico header: Pins 17, 18, 19, 20)
//   Pin 17 = GP13 (SDATA), Pin 18 = GND, Pin 19 = GP14 (BCLK), Pin 20 = GP15 (LRCLK)
//--------------------------------------------------------------------+
#define PIN_I2S_SDATA       13  // PIO Out -> Pi 5 GPIO 20/22/24/26 (SDI0..SDI3)
#define PIN_I2S_BCLK_BASE   14  // PIO In 0 = GPIO 14 (BCLK), PIO In 1 = GPIO 15 (LRCLK/WS)

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

enum {
    VOLUME_CTRL_0_DB = 0,
    VOLUME_CTRL_50_DB = 12800,
};

// Audio Feature Unit state (+1 for Master channel 0)
int8_t mute[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1] = {0};
int16_t volume[CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX + 1] = {0};

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

// Built-in test tone mode (0 = off / normal USB audio; >0 = phase step for test triangle/sine wave)
static volatile uint16_t test_tone_freq_hz = 0;
static uint32_t test_tone_phase = 0;

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

    // Configure DMA Channel A (chains to B)
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

    // Configure DMA Channel B (chains to A)
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

    // Start DMA Channel A (will wait on PIO TX DREQ until Pi 5 BCLK/LRCLK starts)
    dma_channel_start(dma_ch_a);
}

//--------------------------------------------------------------------+
// Unpack 24-bit USB audio (S24_3LE) into 32-bit MSB-aligned I2S buffer
//--------------------------------------------------------------------+
static void fill_i2s_buffer_from_usb(uint32_t *dst_words) {
    uint16_t bytes_read = tud_audio_read(usb_raw_bytes, USB_FRAME_BYTES);

    if (test_tone_freq_hz > 0) {
        // Generate deterministic full-scale -12 dBFS triangle wave on Left & inverted on Right
        uint32_t phase_inc = (uint32_t)(((uint64_t)test_tone_freq_hz << 32) / SAMPLE_RATE_HZ);
        for (uint32_t i = 0; i < I2S_DMA_FRAMES; i++) {
            test_tone_phase += phase_inc;
            uint32_t folded = (test_tone_phase & 0x80000000u) ? ~test_tone_phase : test_tone_phase;
            int32_t tri = (int32_t)((folded << 1) - 0x80000000u);
            int32_t sample = (tri >> 2) & 0xFFFFFF00; // -12 dBFS, 24-bit MSB aligned
            dst_words[i * 2 + 0] = (uint32_t)sample;
            dst_words[i * 2 + 1] = (uint32_t)(-sample);
        }
        return;
    }

    bool is_muted = mute[0] || (mute[1] && mute[2]);
    if (bytes_read == 0 || is_muted) {
        memset(dst_words, 0, I2S_DMA_WORDS * sizeof(uint32_t));
        return;
    }

    uint32_t frames_received = bytes_read / 6; // 6 bytes per stereo frame (3B Left + 3B Right)
    const uint8_t *src = usb_raw_bytes;

    for (uint32_t i = 0; i < frames_received && i < I2S_DMA_FRAMES; i++) {
        // Left channel 24-bit little-endian -> 32-bit MSB-aligned (bits 31..8)
        uint32_t left = ((uint32_t)src[0] << 8) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 24);
        // Right channel 24-bit little-endian -> 32-bit MSB-aligned (bits 31..8)
        uint32_t right = ((uint32_t)src[3] << 8) | ((uint32_t)src[4] << 16) | ((uint32_t)src[5] << 24);

        dst_words[i * 2 + 0] = mute[1] ? 0 : left;
        dst_words[i * 2 + 1] = mute[2] ? 0 : right;
        src += 6;
    }

    // Zero-pad any remaining frames if the USB FIFO had slightly fewer than 48 frames
    for (uint32_t i = frames_received; i < I2S_DMA_FRAMES; i++) {
        dst_words[i * 2 + 0] = 0;
        dst_words[i * 2 + 1] = 0;
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

    // Fallback: if external BCLK/LRCLK is not active yet (>2ms since last DMA completion),
    // drain the USB FIFO at 1ms intervals so the host OS can stream audio before I2S wires are attached.
    if ((now_ms - last_dma_activity_ms) > 2) {
        if (now_ms != fallback_ms) {
            fallback_ms = now_ms;
            fill_i2s_buffer_from_usb(i2s_dma_buf_a);
        }
    }
}

//--------------------------------------------------------------------+
// Main Loop
//--------------------------------------------------------------------+
int main(void) {
    board_init();
    i2s_slave_init();

    tusb_rhport_init_t dev_init = {
        .role = TUSB_ROLE_DEVICE,
        .speed = TUSB_SPEED_AUTO
    };
    tusb_init(BOARD_TUD_RHPORT, &dev_init);

    if (board_init_after_tusb) {
        board_init_after_tusb();
    }

    // Enable 2-second hardware watchdog so any unexpected fault/lockup auto-recovers cleanly
    watchdog_enable(2000, 1);

    while (1) {
        watchdog_update();
        tud_task();
        audio_task();
    }
}

//--------------------------------------------------------------------+
// Vendor Control Request Callback (Remote BOOTSEL, Test Tone & Hardware Diagnostics)
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
        if (request->bRequest == PIMIX_VENDOR_REQ_FORCE_SDATA) {
            // wValue: 0 = normal PIO mode on GPIO 9, 1 = force GPIO 9 LOW, 2 = force GPIO 9 HIGH
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

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE && request->bRequest == AUDIO_CS_REQ_CUR) {
        audio_control_cur_1_t mute1 = { .bCur = mute[request->bChannelNumber] };
        return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &mute1, sizeof(mute1));
    } else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        if (request->bRequest == AUDIO_CS_REQ_RANGE) {
            audio_control_range_2_n_t(1) range_vol = {
                .wNumSubRanges = tu_htole16(1),
                .subrange[0] = { .bMin = tu_htole16(-VOLUME_CTRL_50_DB), tu_htole16(VOLUME_CTRL_0_DB), tu_htole16(256) }
            };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &range_vol, sizeof(range_vol));
        } else if (request->bRequest == AUDIO_CS_REQ_CUR) {
            audio_control_cur_2_t cur_vol = { .bCur = tu_htole16(volume[request->bChannelNumber]) };
            return tud_audio_buffer_and_schedule_control_xfer(rhport, (tusb_control_request_t const *)request, &cur_vol, sizeof(cur_vol));
        }
    }
    return false;
}

static bool tud_audio_feature_unit_set_request(uint8_t rhport, audio_control_request_t const *request, uint8_t const *buf) {
    (void)rhport;
    TU_ASSERT(request->bEntityID == UAC2_ENTITY_FEATURE_UNIT);
    TU_VERIFY(request->bRequest == AUDIO_CS_REQ_CUR);

    if (request->bControlSelector == AUDIO_FU_CTRL_MUTE) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_1_t));
        mute[request->bChannelNumber] = ((audio_control_cur_1_t const *)buf)->bCur;
        return true;
    } else if (request->bControlSelector == AUDIO_FU_CTRL_VOLUME) {
        TU_VERIFY(request->wLength == sizeof(audio_control_cur_2_t));
        volume[request->bChannelNumber] = ((audio_control_cur_2_t const *)buf)->bCur;
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
