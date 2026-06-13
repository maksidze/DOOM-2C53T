/*
 * Continuous dual-DAC output for the AT32F403A.
 *
 * The AT32 flexible DMA mux must explicitly route the DAC2 request to
 * DMA1 channel 1. A word write to DDTH12R updates DAC1 (PA4) and DAC2
 * (PA5) together, which also lets us probe both possible board audio paths.
 */

#include "dac_output.h"
#include "at32f403a_407.h"
#include "at32f403a_407_dma.h"

#define APB1_CLOCK  (system_core_clock / 2U)
#define DAC_MID_PAIR ((2048U << 16) | 2048U)

static uint32_t dac_buffer[DAC_BUFFER_SIZE];
static volatile bool running;

static void tmr6_set_rate(uint32_t sample_rate)
{
    uint16_t prescaler = 0;
    uint32_t period;

    if (sample_rate == 0) {
        sample_rate = 1;
    }

    period = APB1_CLOCK / sample_rate;
    if (period > 65536U) {
        prescaler = 7;
        period = APB1_CLOCK / (8U * sample_rate);
    }
    if (period > 65536U) {
        prescaler = 63;
        period = APB1_CLOCK / (64U * sample_rate);
    }
    if (period < 1U) period = 1U;
    if (period > 65536U) period = 65536U;

    TMR6->div = prescaler;
    TMR6->pr = (uint16_t)(period - 1U);
    TMR6->swevt_bit.ovfswtr = 1;
}

void dac_output_init(void)
{
    gpio_init_type gpio_cfg;
    dma_init_type dma_cfg;

    crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_DAC_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_TMR6_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
    crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);

    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_pins = GPIO_PINS_4 | GPIO_PINS_5;
    gpio_cfg.gpio_mode = GPIO_MODE_ANALOG;
    gpio_init(GPIOA, &gpio_cfg);

    /* Stock firmware leaves PD3 high; it may enable the analog/audio path. */
    gpio_default_para_init(&gpio_cfg);
    gpio_cfg.gpio_pins = GPIO_PINS_3;
    gpio_cfg.gpio_mode = GPIO_MODE_OUTPUT;
    gpio_cfg.gpio_out_type = GPIO_OUTPUT_PUSH_PULL;
    gpio_cfg.gpio_pull = GPIO_PULL_NONE;
    gpio_cfg.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
    gpio_init(GPIOD, &gpio_cfg);
    gpio_bits_set(GPIOD, GPIO_PINS_3);

    dac_trigger_select(DAC1_SELECT, DAC_TMR6_TRGOUT_EVENT);
    dac_trigger_select(DAC2_SELECT, DAC_TMR6_TRGOUT_EVENT);
    dac_trigger_enable(DAC1_SELECT, TRUE);
    dac_trigger_enable(DAC2_SELECT, TRUE);
    dac_wave_generate(DAC1_SELECT, DAC_WAVE_GENERATE_NONE);
    dac_wave_generate(DAC2_SELECT, DAC_WAVE_GENERATE_NONE);
    dac_output_buffer_enable(DAC1_SELECT, FALSE);
    dac_output_buffer_enable(DAC2_SELECT, FALSE);
    dac_dma_enable(DAC1_SELECT, TRUE);
    dac_dma_enable(DAC2_SELECT, TRUE);
    dac_enable(DAC1_SELECT, FALSE);
    dac_enable(DAC2_SELECT, FALSE);

    for (unsigned int i = 0; i < DAC_BUFFER_SIZE; ++i) {
        dac_buffer[i] = DAC_MID_PAIR;
    }

    dma_reset(DMA1_CHANNEL1);
    dma_default_para_init(&dma_cfg);
    dma_cfg.buffer_size = DAC_BUFFER_SIZE;
    dma_cfg.direction = DMA_DIR_MEMORY_TO_PERIPHERAL;
    dma_cfg.memory_base_addr = (uint32_t)dac_buffer;
    dma_cfg.memory_data_width = DMA_MEMORY_DATA_WIDTH_WORD;
    dma_cfg.memory_inc_enable = TRUE;
    dma_cfg.peripheral_base_addr = (uint32_t)&DAC->ddth12r;
    dma_cfg.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_WORD;
    dma_cfg.peripheral_inc_enable = FALSE;
    dma_cfg.priority = DMA_PRIORITY_HIGH;
    dma_cfg.loop_mode_enable = TRUE;
    dma_init(DMA1_CHANNEL1, &dma_cfg);
    dma_flexible_config(DMA1, FLEX_CHANNEL1, DMA_FLEXIBLE_DAC2);

    TMR6->ctrl1 = 0;
    TMR6->div = 0;
    TMR6->pr = (APB1_CLOCK / 11025U) - 1U;
    TMR6->ctrl2_bit.ptos = 2;
}

void dac_output_start(uint32_t sample_rate)
{
    if (running) {
        dac_output_stop();
    }

    tmr6_set_rate(sample_rate);
    dma_channel_enable(DMA1_CHANNEL1, FALSE);
    dma_data_number_set(DMA1_CHANNEL1, DAC_BUFFER_SIZE);
    DMA1_CHANNEL1->maddr = (uint32_t)dac_buffer;

    dac_enable(DAC1_SELECT, TRUE);
    dac_enable(DAC2_SELECT, TRUE);
    dma_channel_enable(DMA1_CHANNEL1, TRUE);
    TMR6->cval = 0;
    tmr_counter_enable(TMR6, TRUE);
    running = true;
}

void dac_output_stop(void)
{
    tmr_counter_enable(TMR6, FALSE);
    dma_channel_enable(DMA1_CHANNEL1, FALSE);
    dac_enable(DAC1_SELECT, FALSE);
    dac_enable(DAC2_SELECT, FALSE);
    running = false;
}

void dac_output_set_rate(uint32_t sample_rate)
{
    tmr6_set_rate(sample_rate);
}

uint32_t *dac_output_get_buffer(void)
{
    return dac_buffer;
}

uint16_t dac_output_get_position(void)
{
    uint32_t remaining = dma_data_number_get(DMA1_CHANNEL1);

    if (remaining > DAC_BUFFER_SIZE) {
        remaining = DAC_BUFFER_SIZE;
    }
    return (uint16_t)((DAC_BUFFER_SIZE - remaining) & (DAC_BUFFER_SIZE - 1U));
}

bool dac_output_is_running(void)
{
    return running;
}
