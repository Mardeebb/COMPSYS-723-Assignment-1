
/* Standard Includes */
#include <stdio.h>
#include <unistd.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* System Includes */
#include "sys/alt_irq.h"
#include "io.h"
#include <system.h>
#include <altera_avalon_pio_regs.h>
#include "altera_up_avalon_video_character_buffer_with_dma.h"
#include "altera_up_avalon_video_pixel_buffer_dma.h"
#include "altera_up_avalon_ps2.h"
#include <sys/alt_alarm.h>
#include <altera_avalon_uart_regs.h>

/* FreeRTOS Includes */
#include "FreeRTOS/FreeRTOS.h"
#include "FreeRTOS/task.h"
#include "FreeRTOS/queue.h"
#include "FreeRTOS/timers.h"
#include "FreeRTOS/portable.h"
#include "FreeRTOS/portmacro.h"
#include "FreeRTOS/semphr.h"

/* LCD */
#define ESC 27
#define CLEAR_LCD_STRING "[2J"

/* Tasks */
#define   TASK_STACKSIZE       2048
#define DISPLAY_PRIORITY       	11
#define SWITCHES_PRIORITY       12
#define LED_PRIORITY       		  13
#define TIMER_RESET_PRIORITY    14
#define LOAD_MONITOR_PRIORITY   15
#define FREQ_ANALYSER_PRIORITY  16

/* Frequency Plot */
#define FREQPLT_ORI_X 101		//x axis pixel position at the plot origin
#define FREQPLT_GRID_SIZE_X 5	//pixel separation in the x axis between two data points
#define FREQPLT_ORI_Y 199.0		//y axis pixel position at the plot origin
#define FREQPLT_FREQ_RES 20.0	//number of pixels per Hz (y axis scale)
#define ROCPLT_ORI_X 101
#define ROCPLT_GRID_SIZE_X 5
#define ROCPLT_ORI_Y 259.0
#define ROCPLT_ROC_RES 0.5		//number of pixels per Hz/s (y axis scale)
#define MIN_FREQ 45.0

/* Function Prototypes */
static void stabilityMonitorTask(void* pvParameters);
static void LCD_task(void* pvParameters);
static void stabilityMonitorTask(void* pvParameters);
static void pollingSwitchsTask(void* pvParameters);
static void PRVGADraw_Task(void* pvParameters);
void record_time();
void add_time_to_array();

TimerHandle_t timer, timer2;
TaskHandle_t Timer_Reset, Timer_Reset2;
static QueueHandle_t Q_freq_data;
TaskHandle_t  stabilityTaskHandle, loadMonitorTaskHandle, pollingSwitchsTaskHandle, PRVGADraw, LCD_handle;
QueueHandle_t stabilityTaskQueue, loadMonitorTaskQueue;
SemaphoreHandle_t xTimerSemaphore;

typedef struct {
    unsigned int x1;
    unsigned int y1;
    unsigned int x2;
    unsigned int y2;
} Line;

/* Global Variables */
volatile double frequency = 0;
volatile double old_frequency = 0;
volatile double roc_frequency = 0;

volatile int increment_flag = 0;     // 1 when right arrow pressed
volatile int decrement_flag = 0;     // 1 when left arrow pressed 
int change_frequencyTH = 0;
int extended = 0;
int released = 0;

double freqTH = 50.0;
double rocTH = 15.0;

volatile int ms = 0;
volatile int seconds = 0;
volatile int minutes = 0;
volatile int temp_ms = 0;
volatile int temp_seconds = 0;
volatile int temp_minutes = 0;
volatile int min = 0;
volatile int max = 0;
volatile int avg = 0;
int recorded_time[] = { 0,0,0,0,0 };

int loads_active_flag = 0;
int load_state[] = { 0,0,0,0,0 };
int switch_state[] = { 0,0,0,0 };

uint8_t timer500_flag = 1;
uint8_t timer_reset_flag2 = 1;
uint8_t stability_flag = 1;
uint8_t maintenance_flag = 0;
uint8_t stability_changed_flag = 0;

// ------------------------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------------------------- Interupt Service Routines ------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------

void freq_relay_isr() {
    unsigned int samples = IORD(FREQUENCY_ANALYSER_BASE, 0);
    xQueueSendFromISR(stabilityTaskQueue, &samples, pdFALSE);
    portEND_SWITCHING_ISR(pdFALSE);
}

void button_isr() {
    if (maintenance_flag == 1) {
        maintenance_flag = 0;
        IOWR_ALTERA_AVALON_PIO_DATA(SEVEN_SEG_BASE, 0x00000000);
    } else {
        maintenance_flag = 1;
        IOWR_ALTERA_AVALON_PIO_DATA(SEVEN_SEG_BASE, 0xEEEEEEEE); // E -> M -> Maintenance
    }
    IORD_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE);
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE, 0x7);
    return;
}

void ps2_isr(void* ps2_device, alt_u32 id) {
    static int extended = 0;
    static int released = 0;
    unsigned char byte;

    if (alt_up_ps2_read_data_byte_timeout(ps2_device, &byte) != 0) {
        return;
    }
    if (byte == 0xE0) {
        extended = 1;
        return;
    } else if (byte == 0xF0) {
        released = 1;
        return;
    }
    if (extended == 1) {
        if (!released) {
            switch (byte) {
                case 0x72: // Down
                    change_frequencyTH = 0;
                    break;
                case 0x75: // Up
                    change_frequencyTH = 1;
                    break;
                case 0x74: // Right
                    increment_flag = 1;
                    break;
                case 0x6B: // Left
                    decrement_flag = 1;
                    break;
            }
        }
    }
    extended = 0;
    released = 0;
    alt_up_ps2_read_data_byte_timeout(ps2_device, &byte);
}

// ------------------------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------------------------- TASKS --------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------

static void stabilityMonitorTask(void* pvParameters) {
    unsigned int received_samples;
    static uint8_t last_stability = 1;
    while (1) {
        if (xQueueReceive(stabilityTaskQueue, &received_samples, portMAX_DELAY)) {
            old_frequency = frequency;
            frequency = 16000.0 / (double) received_samples;
            roc_frequency = ((frequency - old_frequency) / received_samples) * 16000.0;
            xQueueSend(Q_freq_data, &frequency, pdFALSE);

            if (frequency >= freqTH + 1.0 || frequency <= freqTH - 1.0 || roc_frequency < -1 * rocTH || roc_frequency > rocTH) {
                stability_flag = 0;
                record_time();
                xQueueSend(loadMonitorTaskQueue, &stability_flag, pdFALSE);
                xTaskNotifyGive(loadMonitorTaskHandle);
            } else {
                stability_flag = 1;
                record_time();
                xQueueSend(loadMonitorTaskQueue, &stability_flag, pdFALSE);
                xTaskNotifyGive(loadMonitorTaskHandle);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void loadMonitorTask(void* pvParameters) {
    uint8_t received_stability;
    static uint8_t last_stability = 1;
    unsigned int led_output = 0;
    unsigned int redled_output = 0;

    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (xQueueReceive(loadMonitorTaskQueue, &received_stability, portMAX_DELAY) && xSemaphoreTake(xTimerSemaphore, portMAX_DELAY) && maintenance_flag == 0) {

            if (received_stability != last_stability) {
                stability_changed_flag = 1;
                xTimerReset(timer, 10);
                timer500_flag = 0;
                last_stability = received_stability;
                continue;
            }
            if (timer500_flag && !received_stability) {
                for (int i = 4; i >= 0; i--) {
                    if (load_state[i] == 1) {
                        load_state[i] = 0;
                        loads_active_flag = 1;
                        add_time_to_array();
                        break;
                    }
                }
            }
            if (timer500_flag && received_stability) {
                for (int i = 0; i <= 4; i++) {
                    if (load_state[i] == 0) {
                        load_state[i] = 1;
                        loads_active_flag = 1;
                        add_time_to_array();
                        break;
                    }
                }
            }
        }
        if (maintenance_flag == 1) {
            led_output = 0;
            unsigned int uiSwitchValue = IORD_ALTERA_AVALON_PIO_DATA(SLIDE_SWITCH_BASE);
            for (int i = 0; i < 5; i++) {
                switch_state[i] = (uiSwitchValue >> i) & 0x1;
            }
            for (int i = 0; i < 5; i++) {
                printf("%i of switches is %i\n", i, switch_state[i]);
                led_output |= (switch_state[i] << i);
                printf("LED output is now %i\n", led_output);
                load_state[i] = switch_state[i];
            }
            IOWR_ALTERA_AVALON_PIO_DATA(GREEN_LEDS_BASE, 0);
            IOWR_ALTERA_AVALON_PIO_DATA(RED_LEDS_BASE, led_output);
        }
        if (loads_active_flag) {
            led_output = 0;
            for (int i = 0; i < 5; i++) {
                led_output |= (load_state[i] << i);
            }
            IOWR_ALTERA_AVALON_PIO_DATA(RED_LEDS_BASE, led_output);
            uint32_t green_led_output = (~led_output) & 0x1F;
            IOWR_ALTERA_AVALON_PIO_DATA(GREEN_LEDS_BASE, green_led_output);
            loads_active_flag = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void pollingSwitchsTask(void* pvParameters) {
    unsigned int uiSwitchValue = 0;
    while (1) {
        uiSwitchValue = IORD_ALTERA_AVALON_PIO_DATA(SLIDE_SWITCH_BASE);
        if (maintenance_flag == 1) {
            for (int i = 0; i < 5; i++) {
                load_state[i] = (uiSwitchValue >> i) & 0x1;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void PRVGADraw_Task(void* pvParameters) {
    char bfr_a[30], bfr_b[30], bfr_c[30];
    alt_up_pixel_buffer_dma_dev* pixel_buf;
    pixel_buf = alt_up_pixel_buffer_dma_open_dev(VIDEO_PIXEL_BUFFER_DMA_NAME);
    if (pixel_buf == NULL) {
        printf("can't find pixel buffer device\n");
    }
    alt_up_pixel_buffer_dma_clear_screen(pixel_buf, 0);

    alt_up_char_buffer_dev* char_buf;
    char_buf = alt_up_char_buffer_open_dev("/dev/video_character_buffer_with_dma");
    if (char_buf == NULL) {
        printf("can't find char buffer device\n");
    }
    alt_up_char_buffer_clear(char_buf);

    //Set up plot axes
    alt_up_pixel_buffer_dma_draw_hline(pixel_buf, 100, 590, 200, ((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);
    alt_up_pixel_buffer_dma_draw_hline(pixel_buf, 100, 590, 300, ((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);
    alt_up_pixel_buffer_dma_draw_vline(pixel_buf, 100, 50, 200, ((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);
    alt_up_pixel_buffer_dma_draw_vline(pixel_buf, 100, 220, 300, ((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);

    alt_up_char_buffer_string(char_buf, "Frequency(Hz)", 4, 4);
    alt_up_char_buffer_string(char_buf, "52", 10, 7);
    alt_up_char_buffer_string(char_buf, "50", 10, 12);
    alt_up_char_buffer_string(char_buf, "48", 10, 17);
    alt_up_char_buffer_string(char_buf, "46", 10, 22);

    alt_up_char_buffer_string(char_buf, "df/dt(Hz/s)", 4, 26);
    alt_up_char_buffer_string(char_buf, "60", 10, 28);
    alt_up_char_buffer_string(char_buf, "30", 10, 30);
    alt_up_char_buffer_string(char_buf, "0", 10, 32);
    alt_up_char_buffer_string(char_buf, "-30", 9, 34);
    alt_up_char_buffer_string(char_buf, "-60", 9, 36);

    double freq[100], dfreq[100];
    int i = 99, j = 0;
    Line line_freq, line_roc;

    while (1) {
        while (uxQueueMessagesWaiting(Q_freq_data) != 0) {
            xQueueReceive(Q_freq_data, freq + i, 0);
            if (i == 0) {
                dfreq[0] = (freq[0] - freq[99]) * 2.0 * freq[0] * freq[99] / (freq[0] + freq[99]);
            } else {
                dfreq[i] = (freq[i] - freq[i - 1]) * 2.0 * freq[i] * freq[i - 1] / (freq[i] + freq[i - 1]);
            }
            if (dfreq[i] > 100.0) {
                dfreq[i] = 100.0;
            }
            i = ++i % 100;
        }
        alt_up_pixel_buffer_dma_draw_box(pixel_buf, 101, 0, 639, 199, 0, 0);
        alt_up_pixel_buffer_dma_draw_box(pixel_buf, 101, 201, 639, 299, 0, 0);

        for (j = 0; j < 99; ++j) {
            if (((int) (freq[(i + j) % 100]) > MIN_FREQ) && ((int) (freq[(i + j + 1) % 100]) > MIN_FREQ)) {
                //Frequency plot
                line_freq.x1 = FREQPLT_ORI_X + FREQPLT_GRID_SIZE_X * j;
                line_freq.y1 = (int) (FREQPLT_ORI_Y - FREQPLT_FREQ_RES * (freq[(i + j) % 100] - MIN_FREQ));

                line_freq.x2 = FREQPLT_ORI_X + FREQPLT_GRID_SIZE_X * (j + 1);
                line_freq.y2 = (int) (FREQPLT_ORI_Y - FREQPLT_FREQ_RES * (freq[(i + j + 1) % 100] - MIN_FREQ));

                //Frequency RoC plot
                line_roc.x1 = ROCPLT_ORI_X + ROCPLT_GRID_SIZE_X * j;
                line_roc.y1 = (int) (ROCPLT_ORI_Y - ROCPLT_ROC_RES * dfreq[(i + j) % 100]);

                line_roc.x2 = ROCPLT_ORI_X + ROCPLT_GRID_SIZE_X * (j + 1);
                line_roc.y2 = (int) (ROCPLT_ORI_Y - ROCPLT_ROC_RES * dfreq[(i + j + 1) % 100]);

                if (freq[(i + j) % 100] >= FREQ_TH + 1 || freq[(i + j) % 100] <= FREQ_TH - 1) {
                    alt_up_pixel_buffer_dma_draw_line(pixel_buf, line_freq.x1, line_freq.y1, line_freq.x2, line_freq.y2, 0xFF00 << 0, 0);
                } else {
                    alt_up_pixel_buffer_dma_draw_line(pixel_buf, line_freq.x1, line_freq.y1, line_freq.x2, line_freq.y2, 0x3FF << 0, 0);
                }
                if (dfreq[(i + j) % 100] < -1 * ROC_TH || dfreq[(i + j) % 100] > ROC_TH) {
                    alt_up_pixel_buffer_dma_draw_line(pixel_buf, line_roc.x1, line_roc.y1, line_roc.x2, line_roc.y2, 0xFF00 << 0, 0);
                } else {
                    alt_up_pixel_buffer_dma_draw_line(pixel_buf, line_roc.x1, line_roc.y1, line_roc.x2, line_roc.y2, 0x3ff << 0, 0);
                }
            }
        }

        if (increment_flag) {
            if (maintenance_flag == 1) {
                if (change_frequencyTH == 0) {
                    freqTH += 1.0;
                } else if (change_frequencyTH == 1) {
                    rocTH += 1.0;
                }
            }
            increment_flag = 0;
        }
        if (decrement_flag) {
            if (maintenance_flag == 1) {
                if (change_frequencyTH == 0) {
                    freqTH -= 1.0;
                } else if (change_frequencyTH == 1) {
                    rocTH -= 1.0;
                }
            }
            decrement_flag = 0;
        }
        sprintf(bfr_a, "FREQUENCY: %2.4f Hz     ", frequency);
        sprintf(bfr_b, "ROC:       %2.4f Hz/s   ", roc_frequency);
        alt_up_char_buffer_string(char_buf, bfr_a, 5, 40);
        alt_up_char_buffer_string(char_buf, bfr_b, 5, 42);

        sprintf(bfr_a, "FREQ_TH: %.1f Hz  ", freqTH);
        sprintf(bfr_b, "ROC_TH:  %.4f Hz/s", rocTH);
        alt_up_char_buffer_string(char_buf, bfr_a, 5, 46);
        alt_up_char_buffer_string(char_buf, bfr_b, 5, 48);

        alt_up_char_buffer_string(char_buf, "Stability:    ", 35, 40);
        if (stability_flag == 1) {
            sprintf(bfr_c, "Stable    ");
            alt_up_pixel_buffer_dma_draw_box(pixel_buf, 275, 332, 368, 346, 0x3ff << 0, 0);
        } else {
            sprintf(bfr_c, "Not Stable");
            alt_up_pixel_buffer_dma_draw_box(pixel_buf, 275, 332, 368, 346, 0xff00 << 0, 0);
        }

        alt_up_char_buffer_string(char_buf, "Mode:", 35, 46);
        alt_up_char_buffer_string(char_buf, bfr_c, 35, 42);
        if (maintenance_flag == 1) {
            sprintf(bfr_c, "Maintenance");
        } else {
            sprintf(bfr_c, "Normal     ");
        }
        alt_up_char_buffer_string(char_buf, bfr_c, 35, 48);

        alt_up_char_buffer_string(char_buf, "Running Time:", 50, 40);
        alt_up_char_buffer_string(char_buf, "Reaction Times:", 50, 44);
        sprintf(bfr_a, "%2dm:%2ds:%3dms", minutes, seconds, ms);
        sprintf(bfr_b, "%dms %dms %dms %dms %dms", recorded_time[0], recorded_time[1], recorded_time[2], recorded_time[3], recorded_time[4]);
        alt_up_char_buffer_string(char_buf, bfr_a, 50, 42);
        alt_up_char_buffer_string(char_buf, bfr_b, 50, 46);
        sprintf(bfr_c, "Max: %dms", max);
        alt_up_char_buffer_string(char_buf, bfr_c, 50, 48);
        sprintf(bfr_c, "Min: %dms", min);
        alt_up_char_buffer_string(char_buf, bfr_c, 50, 50);
        sprintf(bfr_c, "AVG: %dms", avg);
        alt_up_char_buffer_string(char_buf, bfr_c, 50, 52);

        alt_up_char_buffer_string(char_buf, "Load state: ", 35, 54);
        for (int i = 0; i < 5; i++) {
            if (maintenance_flag == 0) {
                if (load_state_N[i] == 1) {
                    sprintf(bfr_c, "Load %d: ON ", i);
                    alt_up_pixel_buffer_dma_draw_box(pixel_buf, 100 + i * 120, 443, 130 + i * 120, 458, 0x3ff << 0, 0);
                } else {
                    sprintf(bfr_c, "Load %d: OFF", i);
                    alt_up_pixel_buffer_dma_draw_box(pixel_buf, 100 + i * 120, 443, 130 + i * 120, 458, 0xff00 << 0, 0);
                }
            } else {
                if (load_state_M[i] == 1) {
                    sprintf(bfr_c, "Load %d: ON ", i);
                    alt_up_pixel_buffer_dma_draw_box(pixel_buf, 100 + i * 120, 443, 130 + i * 120, 458, 0x3ff << 0, 0);
                } else {
                    sprintf(bfr_c, "Load %d: OFF", i);
                    alt_up_pixel_buffer_dma_draw_box(pixel_buf, 100 + i * 120, 443, 130 + i * 120, 458, 0xff00 << 0, 0);
                }
            }
            alt_up_char_buffer_string(char_buf, bfr_c, 5 + i * 15, 56);
        }
        vTaskDelay(10);
    }
}

/* Dont use printf here, hogs CPU */
static void LCD_task(void* pvParameters) {
    FILE* lcd;
    lcd = fopen(CHARACTER_LCD_NAME, "w");
    while (1) {
        fprintf(lcd, "%c%s", ESC, CLEAR_LCD_STRING);
        if (maintenance_flag == 1) {
            fprintf(lcd, "Mode: \nMaintenance\n");
        } else {
            fprintf(lcd, "Mode: \nNormal\n");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// ------------------------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------------------------- Timers -------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------

/* Timer for load monitoring */
void Timer_Reset_Task(void* pvParameters) {
    while (1) {
        if (timer500_flag == 1) {
            xTimerReset(timer, 10);
            timer500_flag = 0;
        }
    }
}
void vTimerCallback(xTimerHandle t_timer) {
    timer500_flag = 1;
    xSemaphoreGiveFromISR(xTimerSemaphore, NULL);
}


/* Timer for time recordings */
void Timer_Reset_Task2(void* pvParameters) {
    while (1) {
        if (timer_reset_flag2 == 1) {
            xTimerReset(timer2, 10);
            timer_reset_flag2 = 0;
        }
    }
}
void vTimerCallback2(xTimerHandle t_timer2) {
    timer_reset_flag2 = 1;
    ms++;
    if (ms > 999) {
        ms = 0;
        seconds++;
    }
    if (seconds > 59) {
        minutes++;
        seconds = 0;
    }
}

/* Called after load state is changed */
void add_time_to_array() {
    int total_ms = (minutes * 60 * 1000) + (seconds * 1000) + ms;
    int temp_time = (temp_minutes * 60 * 1000) + (temp_seconds * 1000) + temp_ms;
    int sum = 0;

    for (int i = 1; i < 5; i++) {
        recorded_time[i - 1] = recorded_time[i];
    }
    recorded_time[4] = total_ms - temp_time;
    for (int i = 0; i < 5; i++) {
        if (recorded_time[i] <= min) {
            min = recorded_time[i];
        }
        if (recorded_time[i] >= max) {
            max = recorded_time[i];
        }
        sum += recorded_time[i];
    }
    avg = sum / 5;
}

/* Called after stability flag changes */
void record_time() {
    temp_minutes = minutes;
    temp_seconds = seconds;
    temp_ms = ms;
}

// ------------------------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------
// --------------------------------------------------------------------- Main ---------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------
// ------------------------------------------------------------------------------------------------------------------------------------------

int main(void) {
    stabilityTaskQueue = xQueueCreate(100, sizeof(double));
    loadMonitorTaskQueue = xQueueCreate(100, sizeof(int));
    xTimerSemaphore = xSemaphoreCreateBinary();

    /* Tasks */
    xTaskCreate(stabilityMonitorTask, "stabilityMonitorTask", TASK_STACKSIZE, NULL, FREQ_ANALYSER_PRIORITY, &stabilityTaskHandle);
    xTaskCreate(loadMonitorTask, "loadMonitorTask", TASK_STACKSIZE, NULL, LOAD_MONITOR_PRIORITY, &loadMonitorTaskHandle);
    xTaskCreate(pollingSwitchsTask, "pollingSwitchsTask", TASK_STACKSIZE, NULL, SWITCHES_PRIORITY, &pollingSwitchsTaskHandle);
    xTaskCreate(LCD_task, "LCD_task", TASK_STACKSIZE, NULL, DISPLAY_PRIORITY, &LCD_handle);
    xTaskCreate(PRVGADraw_Task, "DrawTsk", TASK_STACKSIZE, NULL, DISPLAY_PRIORITY, &PRVGADraw);
    xTaskCreate(Timer_Reset_Task, "0", configMINIMAL_STACK_SIZE, NULL, TIMER_RESET_PRIORITY, &Timer_Reset);
    xTaskCreate(Timer_Reset_Task2, "0", configMINIMAL_STACK_SIZE, NULL, TIMER_RESET_PRIORITY, &Timer_Reset2);

    /* ISRs */
    timer500_flag = 1;
    if (ps2_device == NULL) {
        printf("can't find PS/2 device\n");
        return 1;
    }
    timer = xTimerCreate("Timer Name", 500, pdTRUE, NULL, vTimerCallback);
    timer2 = xTimerCreate("Timer Name 2", 1, pdTRUE, NULL, vTimerCallback2);
    Q_freq_data = xQueueCreate(100, sizeof(double));
    alt_irq_register(FREQUENCY_ANALYSER_IRQ, 0, freq_relay_isr);
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE, 0x7);
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(PUSH_BUTTON_BASE, 0x7);
    alt_irq_register(PUSH_BUTTON_IRQ, 0, button_isr);
    IOWR_ALTERA_AVALON_PIO_DATA(SEVEN_SEG_BASE, 0x00000000);
    alt_up_ps2_dev* ps2_device = alt_up_ps2_open_dev(PS2_NAME);
    alt_up_ps2_enable_read_interrupt(ps2_device);
    alt_irq_register(PS2_IRQ, ps2_device, ps2_isr);

    vTaskStartScheduler();
    while (1);
}
