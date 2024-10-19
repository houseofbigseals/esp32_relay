/*
Modified version of dht lib from https://github.com/remixoff/esp32-DHT11
*/

#ifndef DHT_H_
#define DHT_H_

#include "esp_timer.h"
#include "esp_mac.h"
#include <esp_log.h>
#include "driver/gpio.h"
#include "rom/ets_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

enum dht_status {
    DHT_CRC_ERROR = -2,
    DHT_TIMEOUT_ERROR,
    DHT_OK
};

typedef struct dht_reading {
    int status;
    float temperature;
	float humidity;
}dht_reading;

typedef enum{
	MODE_DHT11,
	MODE_DHT22
}dht_mode_t;

static const char* TAGGG =  "dht";

//static gpio_num_t dht_gpio;
//static dht_mode_t dht_mode;
//static int64_t last_read_time = -2000000;
//static struct dht_reading last_read;

/// context to store params of different dht sensors
typedef struct dht_context_t{
    dht_mode_t dht_mode;
    gpio_num_t dht_gpio;
    int64_t last_dht_read_time;//  = -2000000;
    struct dht_reading last_read;
}dht_context_t;


void DHT11_init(dht_context_t* ctx, gpio_num_t);

void DHT22_init(dht_context_t* ctx, gpio_num_t);

dht_reading DHT_read(dht_context_t* ctx);





static int _waitOrTimeout(dht_context_t* ctx, uint16_t microSeconds, int level) {
    int micros_ticks = 0;
    while(gpio_get_level(ctx->dht_gpio) == level) { 
        if(micros_ticks++ > microSeconds) 
            return DHT_TIMEOUT_ERROR;
        ets_delay_us(1);
    }
    return micros_ticks;
}

static int _checkCRC(uint8_t data[]) {
    if(data[4] == ((data[0] + data[1] + data[2] + data[3])&0xFF))
        return DHT_OK;
    else
		ESP_LOGE(TAGGG,"Crc error: %02x != %02x+%02x+%02x+%02x"
				,data[4]
				,data[0]
				,data[1]
				,data[2]
				,data[3]
				);
        return DHT_CRC_ERROR;
}

static void _sendStartSignal(dht_context_t* ctx) {
    gpio_set_direction(ctx->dht_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(ctx->dht_gpio, 0);
    ets_delay_us(20 * 1000);
    gpio_set_level(ctx->dht_gpio, 1);
    ets_delay_us(40);
    gpio_set_direction(ctx->dht_gpio, GPIO_MODE_INPUT);
    
}

static int _checkResponse(dht_context_t* ctx) {
    /* Wait for next step ~80us*/
    if(_waitOrTimeout(ctx, 80, 0) == DHT_TIMEOUT_ERROR)
        return DHT_TIMEOUT_ERROR;

    /* Wait for next step ~80us*/
    if(_waitOrTimeout(ctx, 80, 1) == DHT_TIMEOUT_ERROR) 
        return DHT_TIMEOUT_ERROR;

    return DHT_OK;
}

static dht_reading _timeoutError() {
    dht_reading timeoutError = {DHT_TIMEOUT_ERROR, -1, -1};
    return timeoutError;
}

static dht_reading _crcError() {
    dht_reading crcError = {DHT_CRC_ERROR, -1, -1};
    return crcError;
}

static void DHT_init(dht_context_t* ctx, gpio_num_t gpio_num, dht_mode_t _dht_mode){
    /* Wait 1 seconds to make the device pass its initial unstable status */
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ctx->last_dht_read_time = -2000000;
    ctx->dht_gpio = gpio_num;
	ctx->dht_mode = _dht_mode;
    ESP_LOGI(TAGGG, "Initializing dht sensor: gpio - %d, type - %d.", ctx->dht_gpio, ctx->dht_mode);
}

void DHT11_init(dht_context_t* ctx, gpio_num_t gpio_num) {
	DHT_init(ctx, gpio_num, MODE_DHT11);
}

void DHT22_init(dht_context_t* ctx, gpio_num_t gpio_num) {
	DHT_init(ctx, gpio_num, MODE_DHT22);
}


struct dht_reading DHT_read(dht_context_t* ctx) 
{
    /* Tried to sense too son since last read (dht needs ~2 seconds to make a new read) */
    if(esp_timer_get_time() - 2000000 < ctx->last_dht_read_time) {
        ESP_LOGI(TAGGG, "dht sensor: last read time - %llu.", ctx->last_dht_read_time);
        return ctx->last_read;
    }
    ESP_LOGI(TAGGG, "dht sensor: last read time - %llu.", ctx->last_dht_read_time);
    ctx->last_dht_read_time = esp_timer_get_time();

    uint8_t data[5] = {0,0,0,0,0};

    _sendStartSignal(ctx);

    if(_checkResponse(ctx) == DHT_TIMEOUT_ERROR)
        return ctx->last_read = _timeoutError();
    
    /* Read response */
    for(int i = 0; i < 40; i++) {
        /* Initial data */
        if(_waitOrTimeout(ctx, 50, 0) == DHT_TIMEOUT_ERROR)
            return ctx->last_read = _timeoutError();
                
        if(_waitOrTimeout(ctx, 70, 1) > 28) {
            /* Bit received was a 1 */
            data[i/8] |= (1 << (7-(i%8)));
        }
    }

    if(_checkCRC(data) != DHT_CRC_ERROR) {
        ctx->last_read.status = DHT_OK;
		if (ctx->dht_mode == MODE_DHT11){
	        ctx->last_read.temperature = data[2];
		    ctx->last_read.humidity = data[0];
		} else if (ctx->dht_mode == MODE_DHT22 ) {
			ctx->last_read.temperature = ((data[2]<<8) + data[3])/10;
			ctx->last_read.humidity = ((data[0]<<8) + data[1])/10;
		} else {
		};
        return ctx->last_read;
    } else {
        return ctx->last_read = _crcError();
    }
}


#endif