/*

sigdroid The odroid-go signal / function generator

Hacked together by the BACKOFFICE crew

Chrissy
Dr A
Hopefully Sad Ken!

come talk to us on Discord details on www.youtube.com/backofficeshow

*/

#include "freertos/FreeRTOS.h"
#include "esp_system.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_partition.h"
#include "driver/i2s.h"
#include "esp_spiffs.h"
#include "nvs_flash.h"
#include "esp_sleep.h"
#include "driver/rtc_io.h"
#include "esp_ota_ops.h"

extern "C"
{
#include "../components/odroid/odroid_settings.h"
#include "../components/odroid/odroid_audio.h"
#include "../components/odroid/odroid_input.h"
#include "../components/odroid/odroid_system.h"
#include "../components/odroid/odroid_display.h"
#include "../components/odroid/odroid_sdcard.h"

#include "../components/ugui/ugui.h"
}

#include <dirent.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define ESP32_PSRAM (0x3f800000)

#define AUDIO_SAMPLE_RATE (44100)

QueueHandle_t vidQueue;

#define SIGDROID_WIDTH 160
#define SIGDROID_HEIGHT 250
uint8_t framebuffer[SIGDROID_WIDTH * SIGDROID_HEIGHT];
uint16_t pal16[256];
bool IsPal;

static int AudioSink = ODROID_AUDIO_SINK_DAC;

#define I2S_SAMPLE_RATE   (44100)
#define SAMPLERATE I2S_SAMPLE_RATE // Sample rate of our waveforms in Hz

#define AMPLITUDE     1000
#define WAV_SIZE      256
int32_t sine[WAV_SIZE]     = {0};

void generateSine(int32_t amplitude, int32_t* buffer, uint16_t length) {
  for (int i=0; i<length; ++i) {
    buffer[i] = int32_t(float(amplitude)*sin(2.0*M_PI*(1.0/length)*i));
  }
}

void playWave(int32_t* buffer, uint16_t length, float frequency, float seconds) {
  short outbuf[2];
  uint32_t iterations = seconds*SAMPLERATE;
  float delta = (frequency*length)/float(SAMPLERATE);
  for (uint32_t i=0; i<iterations; ++i) {
    uint16_t pos = uint32_t(i*delta) % length;
    int32_t sample = buffer[pos];
    outbuf[0] = sample;
    outbuf[1] = sample;
    odroid_audio_submit(outbuf, 1);
  }
}

void initSound()
{
  odroid_audio_volume_set(ODROID_VOLUME_LEVEL4);
}

void uninitSound()
{
  odroid_audio_volume_set(ODROID_VOLUME_LEVEL0);
}

void videoTask(void *arg)
{
    while(1)
    {
        uint8_t* param;
        xQueuePeek(vidQueue, &param, portMAX_DELAY);
        memcpy(framebuffer, param, sizeof(framebuffer));
        xQueueReceive(vidQueue, &param, portMAX_DELAY);

    }

    odroid_display_lock_sms_display();

    // Draw hourglass
    odroid_display_show_hourglass();

    odroid_display_unlock_sms_display();

    vTaskDelete(NULL);

    while (1) {}
}


UG_GUI gui;
uint16_t* fb;

static void pset(UG_S16 x, UG_S16 y, UG_COLOR color)
{
    fb[y * 320 + x] = color;
}

static void window1callback(UG_MESSAGE* msg)
{
}

static void UpdateDisplay()
{
    UG_Update();
    ili9341_write_frame_rectangleLE(0, 0, 320, 240, fb);
}

#define MAX_OBJECTS 20
#define ITEM_COUNT  10
#define TRUE 1
#define FALSE 0

UG_WINDOW window1;
UG_BUTTON button1;
UG_TEXTBOX textbox[ITEM_COUNT];
UG_OBJECT objbuffwnd1[MAX_OBJECTS];

void sigdroid_init()
{
    float frequency = 1000;
    bool playsound = FALSE;
    printf("%s: HEAP:0x%x (%#08x)\n",
      __func__,
      esp_get_free_heap_size(),
      heap_caps_get_free_size(MALLOC_CAP_DMA));

    const char* result = NULL;
    static const size_t MAX_DISPLAY_LENGTH = 38;

    fb = (uint16_t*)heap_caps_malloc(320 * 240 * 2, MALLOC_CAP_SPIRAM);
    if (!fb) abort();

    UG_Init(&gui, pset, 320, 240);

    UG_WindowCreate(&window1, objbuffwnd1, MAX_OBJECTS, window1callback);

    UG_WindowSetTitleText(&window1, "sigDroid-go");
    UG_WindowSetTitleTextFont(&window1, &FONT_10X16);
    UG_WindowSetTitleTextAlignment(&window1, ALIGN_CENTER);

    UG_WindowShow(&window1);
    UpdateDisplay();

    odroid_gamepad_state previousState;
    odroid_input_gamepad_read(&previousState);

    while (true)
    {
       odroid_gamepad_state state;
       odroid_input_gamepad_read(&state);

       if (playsound == TRUE)
       {
          playWave(sine, WAV_SIZE, frequency, 0.01);
       }else{
          vTaskDelay(10 / portTICK_PERIOD_MS);
       }

       if (!previousState.values[ODROID_INPUT_MENU] && state.values[ODROID_INPUT_MENU])
       {
            esp_restart();
       }else if (!previousState.values[ODROID_INPUT_A] && state.values[ODROID_INPUT_A])
       {
            initSound();
            playsound = TRUE;
       }else if (!previousState.values[ODROID_INPUT_B] && state.values[ODROID_INPUT_B])
       {
            uninitSound();
            playsound = FALSE;
       }else if (state.values[ODROID_INPUT_UP])
       {
            frequency = frequency + 10;
       }else if (state.values[ODROID_INPUT_DOWN])
       {
            frequency = frequency - 10;
       }else if (state.values[ODROID_INPUT_LEFT])
       {
            frequency = frequency - 100;
       }else if (state.values[ODROID_INPUT_RIGHT])
       {
            frequency = frequency + 100;                                    
       }else if (!previousState.values[ODROID_INPUT_VOLUME] && state.values[ODROID_INPUT_VOLUME])
       {
          odroid_audio_terminate();
          if (AudioSink == ODROID_AUDIO_SINK_DAC){
            AudioSink = ODROID_AUDIO_SINK_SPEAKER;
            odroid_audio_init(ODROID_AUDIO_SINK_SPEAKER, AUDIO_SAMPLE_RATE);
          }else if (AudioSink == ODROID_AUDIO_SINK_SPEAKER){
            AudioSink = ODROID_AUDIO_SINK_DAC;
            odroid_audio_init(ODROID_AUDIO_SINK_DAC, AUDIO_SAMPLE_RATE);
          }
          odroid_audio_volume_set(ODROID_VOLUME_LEVEL0);
       }

       previousState = state;
    }

}

void sigdroid_step(odroid_gamepad_state* gamepad)
{

}

bool RenderFlag;
extern "C" void app_main()
{
    printf("sigdroid-go started.\n");

    printf("HEAP:0x%x (%#08x)\n",
    esp_get_free_heap_size(),
    heap_caps_get_free_size(MALLOC_CAP_DMA));


    nvs_flash_init();

    odroid_system_init();
    odroid_input_gamepad_init();
    odroid_input_battery_level_init();

    ili9341_prepare();

    ili9341_init();
    ili9341_clear(0x0000);

    generateSine(AMPLITUDE, sine, WAV_SIZE);

    odroid_audio_init(ODROID_AUDIO_SINK_DAC, AUDIO_SAMPLE_RATE);
    odroid_audio_volume_set(ODROID_VOLUME_LEVEL0);

    sigdroid_init();
}


