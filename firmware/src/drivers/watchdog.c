/*
 * OpenScope 2C53T - Watchdog & System Health Monitor
 *
 * Cooperative watchdog pattern:
 *   1. Each task registers with health_register() at startup
 *   2. Each task calls health_checkin() in its main loop
 *   3. A timer periodically calls health_check(), which:
 *      - Updates stack high water marks
 *      - Checks if any task has missed its deadline
 *      - Feeds the FWDGT only if all tasks are healthy
 *   4. If any task is stalled, the FWDGT starves and resets the MCU
 *
 * The FWDGT timeout (~3s) is deliberately longer than the health check
 * interval so the monitor has time to detect and report the problem.
 */

#include "watchdog.h"
#include "lcd.h"
#include <string.h>
#include <stdio.h>
#include <string.h>

#ifndef EMULATOR_BUILD
#include "at32f403a_407_wdt.h"
#endif

/* ═══════════════════════════════════════════════════════════════════
 * Health monitor state
 * ═══════════════════════════════════════════════════════════════════ */

static task_health_t health_tasks[HEALTH_MAX_TASKS];
static int health_count = 0;

/* ═══════════════════════════════════════════════════════════════════
 * Watchdog (FWDGT) — low-level
 * ═══════════════════════════════════════════════════════════════════ */

void watchdog_init(void)
{
#ifndef EMULATOR_BUILD
    /*
     * FWDGT clock = IRC40K (~40kHz).
     * Prescaler DIV64: 40000 / 64 = 625 Hz per tick.
     * Reload 1875: 1875 / 625 = 3.0 seconds timeout.
     *
     * This gives us a 3-second window. The health monitor runs every
     * 500ms and feeds the WDT if all tasks are healthy, so under
     * normal operation the WDT is fed ~6 times per timeout period.
     */
    wdt_register_write_enable(TRUE);
    wdt_divider_set(WDT_CLK_DIV_64);
    wdt_reload_value_set(1875);
    wdt_counter_reload();
    wdt_enable();
#endif
}

void watchdog_feed(void)
{
#ifndef EMULATOR_BUILD
    wdt_counter_reload();
#endif
}

/* ═══════════════════════════════════════════════════════════════════
 * Task Health Monitor
 * ═══════════════════════════════════════════════════════════════════ */

int health_register(const char *name, TaskHandle_t handle)
{
    if (health_count >= HEALTH_MAX_TASKS)
        return -1;

    int slot = health_count;
    health_tasks[slot].name         = name;
    health_tasks[slot].handle       = handle;
    health_tasks[slot].last_checkin = xTaskGetTickCount();
    health_tasks[slot].stack_hwm    = 0;
    health_tasks[slot].registered   = true;
    health_count++;
    return slot;
}

void health_checkin(int slot)
{
    if (slot >= 0 && slot < HEALTH_MAX_TASKS && health_tasks[slot].registered) {
        health_tasks[slot].last_checkin = xTaskGetTickCount();
    }
}

bool health_is_stalled(int slot)
{
    if (slot < 0 || slot >= HEALTH_MAX_TASKS || !health_tasks[slot].registered)
        return false;

    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed = now - health_tasks[slot].last_checkin;
    return elapsed > pdMS_TO_TICKS(HEALTH_DEADLINE_MS);
}

void health_check(void)
{
    bool all_healthy = true;

    for (int i = 0; i < health_count; i++) {
        if (!health_tasks[i].registered)
            continue;

        /* Update stack high water mark */
        health_tasks[i].stack_hwm = uxTaskGetStackHighWaterMark(
            health_tasks[i].handle);

        /* Check deadline */
        if (health_is_stalled(i)) {
            all_healthy = false;
        }
    }

    if (all_healthy) {
        watchdog_feed();
    }
    /* If not all healthy, we deliberately don't feed the watchdog.
     * The FWDGT will reset the MCU after its timeout expires. */
}

const task_health_t *health_get_tasks(void)
{
    return health_tasks;
}

int health_get_count(void)
{
    return health_count;
}

/* ═══════════════════════════════════════════════════════════════════
 * Fault display — red screen of death
 * ═══════════════════════════════════════════════════════════════════ */

void fault_display(const char *title, const char *detail)
{
    /*
     * This runs outside FreeRTOS (interrupts disabled or from a hook).
     * Write directly to LCD — no queues, no RTOS calls.
     */
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_RED);

    /* Title bar */
    lcd_fill_rect(0, 0, LCD_WIDTH, 30, 0x8000); /* Dark red */
    lcd_draw_string(10, 8, "SYSTEM FAULT", COLOR_WHITE, 0x8000);

    /* Error details */
    lcd_draw_string(10, 50, title, COLOR_WHITE, COLOR_RED);

    if (detail != NULL) {
        lcd_draw_string(10, 80, detail, COLOR_YELLOW, COLOR_RED);
    }

    lcd_draw_string(10, 130, "The system will restart", COLOR_WHITE, COLOR_RED);
    lcd_draw_string(10, 155, "automatically in ~3 seconds.", COLOR_WHITE, COLOR_RED);

    lcd_draw_string(10, 200, "If this persists, hold POWER",
                    COLOR_GRAY, COLOR_RED);
    lcd_draw_string(10, 218, "to force shutdown.",
                    COLOR_GRAY, COLOR_RED);

    /*
     * Don't loop forever here — let the watchdog do the reset.
     * The FWDGT is running on its own clock and will fire regardless.
     * Spin just long enough for the user to see the screen.
     */
    volatile uint32_t count = 120000 * 2000; /* ~2 seconds at 120MHz */
    while (count--) {
        __asm volatile("nop");
    }
}

/* fault_display_line — called by configASSERT to show which file:line triggered */
void fault_display_line(const char *file, int line)
{
    /* Convert line number to string (no snprintf to avoid stdlib dep) */
    char line_buf[24];
    int i = 0;
    if (line <= 0) {
        line_buf[i++] = '0';
    } else {
        char tmp[12];
        int n = 0;
        int v = line;
        while (v > 0) { tmp[n++] = '0' + (v % 10); v /= 10; }
        while (n-- > 0) line_buf[i++] = tmp[n];
    }
    line_buf[i] = '\0';

    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_RED);
    lcd_fill_rect(0, 0, LCD_WIDTH, 30, 0x8000);
    lcd_draw_string(10, 8,  "FREERTOS ASSERT", COLOR_WHITE, 0x8000);
    lcd_draw_string(10, 50, file,     COLOR_YELLOW, COLOR_RED);
    lcd_draw_string(10, 80, line_buf, COLOR_WHITE,  COLOR_RED);

    volatile uint32_t count = 120000 * 2000;
    while (count--) { __asm volatile("nop"); }
}

/* ═══════════════════════════════════════════════════════════════════
 * CPU Exception Handlers
 * ═══════════════════════════════════════════════════════════════════ */

void hard_fault_handler_c(uint32_t *fault_stack)
{
    uint32_t stacked_lr = fault_stack[5];
    uint32_t stacked_pc = fault_stack[6];
    uint32_t cfsr = SCB->CFSR;
    uint32_t bfar = SCB->BFAR;

    char pc_buf[48];
    char fault_buf[48];
    snprintf(pc_buf, sizeof(pc_buf), "PC:%08lX LR:%08lX", stacked_pc, stacked_lr);
    snprintf(fault_buf, sizeof(fault_buf), "CFSR:%08lX BFAR:%08lX", cfsr, bfar);

    __disable_irq();
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, COLOR_RED);
    lcd_fill_rect(0, 0, LCD_WIDTH, 30, 0x8000);
    lcd_draw_string(10, 8, "HARD FAULT", COLOR_WHITE, 0x8000);
    lcd_draw_string(10, 52, pc_buf, COLOR_YELLOW, COLOR_RED);
    lcd_draw_string(10, 82, fault_buf, COLOR_WHITE, COLOR_RED);
    lcd_draw_string(10, 132, "Report both lines", COLOR_WHITE, COLOR_RED);
    lcd_draw_string(10, 162, "Auto restart in ~3 sec", COLOR_GRAY, COLOR_RED);
    for (;;) {}
}

void HardFault_Handler(void) __attribute__((naked));
void HardFault_Handler(void)
{
    __asm volatile
    (
        " tst lr, #4                                                \n"
        " ite eq                                                    \n"
        " mrseq r0, msp                                             \n"
        " mrsne r0, psp                                             \n"
        " b hard_fault_handler_c                                    \n"
    );
}


void MemManage_Handler(void)
{
    __disable_irq();
    fault_display("MEM MANAGE FAULT", "Memory access violation");
    for (;;) {}
}

void BusFault_Handler(void)
{
    __disable_irq();
    fault_display("BUS FAULT", "Bus access error");
    for (;;) {}
}

void UsageFault_Handler(void)
{
    __disable_irq();
    fault_display("USAGE FAULT", "Invalid instruction/state");
    for (;;) {}
}
