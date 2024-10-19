/* Simple HTTP Server Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <sys/param.h>
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "protocol_examples_utils.h"
#include "esp_tls_crypto.h"
#include <esp_http_server.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_check.h"
#include <esp_wifi.h>
#include <esp_system.h>
#include "nvs_flash.h"
#include "esp_eth.h"
#include "esp_wireguard.h"
#include "esp_sntp.h"
#include "freertos/queue.h"

#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "http_server.h"
#include "dht_driver.h"



static const char *MTAG = "relay_board";
static wireguard_config_t wg_config = ESP_WIREGUARD_CONFIG_DEFAULT();

// set dht sensors params
//dht_context_t dht11_ctx;
//dht_context_t dht22_ctx;
const gpio_num_t dht11_gpio = GPIO_NUM_8;
const gpio_num_t dht22_gpio = GPIO_NUM_41;
// queue for data from dht
//QueueHandle_t dht11_queue;   // will store only one number for now
//QueueHandle_t dht22_queue;   // will store only one number for now



// set user indication led
gpio_num_t led_gpio = 21;

// Task to read DHT data and store it in the queue
void dht_task(void *pvParameter)
{
    esp_err_t dht_err;
    float dht11_temp = 0;
    float dht11_hum = 0;
    float dht22_temp = 0;
    float dht22_hum = 0;


    while (1)
    {
        dht_err = dht_read_float_data(DHT_TYPE_DHT11, dht11_gpio, &dht11_hum, &dht11_temp);
        ESP_LOGI("dht11", "DHT11 Reading - Status: %d, Temperature: %f, Humidity: %f\n", dht_err, dht11_temp, dht11_hum);
        dht_err = dht_read_float_data(DHT_TYPE_AM2301, dht22_gpio, &dht22_hum, &dht22_temp);
        ESP_LOGI("dht22", "DHT22 Reading - Status: %d, Temperature: %f, Humidity: %f\n", dht_err, dht22_temp, dht22_hum);

        // update data in global state struct
        if (xSemaphoreTake(device_state_mutex, portMAX_DELAY) == pdTRUE)
        {
            global_device_state.uptime = esp_timer_get_time()/1000000; // Cast to double for int64_t
            global_device_state.ext_temp = dht11_temp;
            global_device_state.ext_hum = dht11_hum;
            global_device_state.int_temp = dht22_temp;
            global_device_state.int_hum = dht22_hum;
            // Release the mutex after accessing the shared data
            xSemaphoreGive(device_state_mutex);
        }
        // Wait for 4 seconds before the next reading
        vTaskDelay(2500 / portTICK_PERIOD_MS);
    }
}


void app_main(void)
{
    //relays gpio init
    init_relays();
    // dht11 and dht22 test
    float temp = 0;
    float hum = 0;
    esp_err_t dht_err = dht_read_float_data(DHT_TYPE_DHT11, dht11_gpio, &hum, &temp);
    ESP_LOGI("dht11", "DHT11 Reading - Status: %d, Temperature: %f, Humidity: %f\n",
        dht_err, temp, hum);
    dht_err = dht_read_float_data(DHT_TYPE_AM2301, dht22_gpio, &hum, &temp);
    ESP_LOGI("dht22", "DHT22 Reading - Status: %d, Temperature: %f, Humidity: %f\n",
               dht_err, temp, hum);


    // fill device_state with default values
    global_device_state.ch0 = 1;
    global_device_state.ch1 = 1;
    global_device_state.ch2 = 1;
    global_device_state.ch3 = 1;  // they are inverted
    global_device_state.pcb_temp = 0;
    global_device_state.roots_temp = 0;
    global_device_state.ext_temp = 0;
    global_device_state.ext_hum = 0;
    global_device_state.int_temp = 0;
    global_device_state.int_hum = 0;
    global_device_state.uptime = esp_timer_get_time()/1000000; // mks to seconds

    // Create the mutex before using it
    device_state_mutex = xSemaphoreCreateMutex();

    xTaskCreate(&dht_task, "dht_task", 4096, NULL, tskIDLE_PRIORITY, NULL);

    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    /* Register event handlers to stop the server when Wi-Fi or Ethernet is disconnected,
     * and re-start it upon connection.
     */
#if !CONFIG_IDF_TARGET_LINUX
#ifdef CONFIG_EXAMPLE_CONNECT_WIFI
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_WIFI
#ifdef CONFIG_EXAMPLE_CONNECT_ETHERNET
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &connect_handler, &server));
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ETHERNET_EVENT_DISCONNECTED, &disconnect_handler, &server));
#endif // CONFIG_EXAMPLE_CONNECT_ETHERNET
#endif // !CONFIG_IDF_TARGET_LINUX



    // start connection to wireguard
    esp_err_t err;
    wireguard_ctx_t ctx = {0};

    // very important time sync with sntp  ???
    // https://github.com/trombik/esp_wireguard/issues/29#issuecomment-1331375949
    // https://docs.espressif.com/projects/esp-idf/en/v4.4.3/esp32/api-reference/system/system_time.html#sntp-time-synchronization
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    // 
    ESP_LOGI(MTAG, "Initializing WireGuard.");
    wg_config.private_key = CONFIG_WG_PRIVATE_KEY;
    wg_config.listen_port = CONFIG_WG_LOCAL_PORT;
    wg_config.public_key = CONFIG_WG_PEER_PUBLIC_KEY;
    if (strcmp(CONFIG_WG_PRESHARED_KEY, "") != 0) {
        wg_config.preshared_key = CONFIG_WG_PRESHARED_KEY;
    } else {
        wg_config.preshared_key = NULL;
    }
    wg_config.address= CONFIG_WG_LOCAL_IP_ADDRESS;
    wg_config.netmask = CONFIG_WG_LOCAL_IP_NETMASK;
    wg_config.endpoint = CONFIG_WG_PEER_ADDRESS;
    wg_config.port = CONFIG_WG_PEER_PORT;
    wg_config.persistent_keepalive = CONFIG_WG_PERSISTENT_KEEP_ALIVE;

    ESP_ERROR_CHECK(esp_wireguard_init(&wg_config, &ctx));

    ESP_LOGI(MTAG, "Connecting to the peer.");
    // IMPORTANT NOTE:
    // https://github.com/trombik/esp_wireguard/issues/33
    /*
    TODO:
    So if we activate CONFIG_ESP_NETIF_BRIDGE_EN or CONFIG_LWIP_PPP_SUPPORT in idf.py menuconfig, it will fix the problem.
    For a test, I tried activating "Enable PPP support (new/experimental)" (Name: LWIP_PPP_SUPPORT), and it works.
    So in short, activate CONFIG_LWIP_PPP_SUPPORT in your project configuration in ESP-IDF-v5, recompile your project, and the wireguard tunnel will work as intended.
    */
    //ESP_ERROR_CHECK(esp_wireguard_connect(&ctx));
    // wait to really connect to wg server
   /* while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        err = esp_wireguardif_peer_is_up(&ctx);
        if (err == ESP_OK) {
            ESP_LOGI(MTAG, "Peer is up");
            break;
        } else {
            ESP_LOGI(MTAG, "Peer is down");
        }
    }*/

    //ESP_LOGI(MTAG, "Peer is up !!!!");
    //ESP_ERROR_CHECK(esp_wireguard_set_default(&ctx));
    // NOTE
    // for some reason https://github.com/droscy/esp_wireguard contains strange feature - user set mask changed to 255.255.255.255
    // with this param nothing works, so we have to add correct addr with correct mask 255.255.255.0
    // ESP_ERROR_CHECK(esp_wireguard_add_allowed_ip(&ctx, "10.10.0.5", "255.255.255.0"));
    // or change that directly in lib, which i already did

    /* Start the server for the first time */
    server = start_webserver();
    time_t last_handshake_time = 0;

    while (server) {
        sleep(10);

        //esp_wireguard_latest_handshake(&ctx, &last_handshake_time);
        //ESP_LOGI(MTAG, "last handshake time %lld", last_handshake_time);
        //ESP_ERROR_CHECK(esp_wireguard_connect(&ctx));
    }
}
