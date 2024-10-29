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
#include "mdns.h"
#include "onewire_bus_impl_rmt.h"
#include "ds18b20.h"


static const char *MTAG = "relay_board";
const char* onewire_tag = "onewire";
static wireguard_config_t wg_config = ESP_WIREGUARD_CONFIG_DEFAULT();

// set dht sensors params
const gpio_num_t dht11_gpio = GPIO_NUM_8;
const gpio_num_t dht22_gpio = GPIO_NUM_18;

// set user indication led
gpio_num_t led_gpio = 21;

// i-wire global variables
// install 1-wire bus
#define EXAMPLE_ONEWIRE_BUS_GPIO    45
#define EXAMPLE_ONEWIRE_MAX_DS18B20 2
onewire_bus_handle_t bus = NULL;
onewire_bus_config_t bus_config = {
    .bus_gpio_num = EXAMPLE_ONEWIRE_BUS_GPIO,
};

int ds18b20_device_num = 0;
ds18b20_device_handle_t ds18b20s[EXAMPLE_ONEWIRE_MAX_DS18B20];
ds18b20_device_handle_t pcb_sensor;  // 96092426B912FF28
ds18b20_device_handle_t lamp_sensor;
onewire_device_iter_handle_t iter = NULL;
onewire_device_t next_onewire_device;



void init_ds18b20()
{
    // https://esp-idf-lib.readthedocs.io/en/latest/groups/onewire.html
    // https://esp-idf-lib.readthedocs.io/en/latest/groups/ds18x20.html

    // example fully stolen from https://components.espressif.com/components/espressif/ds18b20/versions/0.1.1
    // but to work we need to include 
    // #include "onewire_bus_impl_rmt.h"
    // #include "ds18b20.h"

    bool built_in_found = false;

    onewire_bus_rmt_config_t rmt_config = {
    .max_rx_bytes = 10, // 1byte ROM command + 8byte ROM number + 1byte device command
    };
    ESP_ERROR_CHECK(onewire_new_bus_rmt(&bus_config, &rmt_config, &bus));
    esp_err_t search_result = ESP_OK;

    // create 1-wire device iterator, which is used for device search
    ESP_ERROR_CHECK(onewire_new_device_iter(bus, &iter));
    ESP_LOGI(onewire_tag, "Device iterator created, start searching...");
    do {
        search_result = onewire_device_iter_get_next(iter, &next_onewire_device);
        if (search_result == ESP_OK) { // found a new device, let's check if we can upgrade it to a DS18B20
            ds18b20_config_t ds_cfg = {};
            // check if the device is a DS18B20, if so, return the ds18b20 handle
            if (ds18b20_new_device(&next_onewire_device, &ds_cfg, &ds18b20s[ds18b20_device_num]) == ESP_OK) {
                ESP_LOGI(onewire_tag, "Found a DS18B20[%d], address: %016llX", ds18b20_device_num, next_onewire_device.address);
                ds18b20_device_num++;

            } else {
                ESP_LOGI(onewire_tag, "Found an unknown device, address: %016llX", next_onewire_device.address);
            }
        }
    } while (search_result != ESP_ERR_NOT_FOUND);
    ESP_ERROR_CHECK(onewire_del_device_iter(iter));
    ESP_LOGI(onewire_tag, "Searching done, %d DS18B20 device(s) found", ds18b20_device_num);

    // Now you have the DS18B20 sensor handle, you can use it to read the temperature
    /*if (CONFIG_PCB_SENSOR_ADDR == "")
    {
        pcb_sensor = ds18b20s[0];
    }
                    if ((!buil_in_found)&&(CONFIG_PCB_SENSOR_ADDR == "unknown"))    // REMOVE IT ON CODE FOR PRODUCTION PCB
                {
                    // it means that it is test run to check addr of built-in sensor address
                    uint64_t builtin_addr = 

                }
    */
}

// to get data from 0 device
float read_roots_temp()
{
    float temperature = 0;
    ESP_ERROR_CHECK(ds18b20_trigger_temperature_conversion(ds18b20s[0]));
    ESP_ERROR_CHECK(ds18b20_get_temperature(ds18b20s[0], &temperature));
    ESP_LOGI(TAG, "temperature read from root DS18B20: %.2fC", temperature);
    return temperature;
}

// just to show data from all sensors to uart
void test_read_ds18b20_data()
{
    float temperature = 0;
    for (int i = 0; i < ds18b20_device_num; i ++) {
        ESP_ERROR_CHECK(ds18b20_trigger_temperature_conversion(ds18b20s[i]));
        ESP_ERROR_CHECK(ds18b20_get_temperature(ds18b20s[i], &temperature));
        ESP_LOGI(TAG, "temperature read from DS18B20[%d]: %.2fC", i, temperature);
    }
}


// Task to read DHT data and store it in the queue
void dht_task(void *pvParameter)
{
    esp_err_t dht_err;
    float dht11_temp = 0;
    float dht11_hum = 0;
    float dht22_temp = 0;
    float dht22_hum = 0;
    float ds18b20_temp_ = 0;


    while (1)
    {
        dht11_temp = -255;  // error indication value (because we have no any normal text error codes)
        dht11_hum = -255;
        dht22_temp = -255;
        dht22_hum = -255;
        ds18b20_temp_ = -255;
        dht_err = dht_read_float_data(DHT_TYPE_DHT11, dht11_gpio, &dht11_hum, &dht11_temp);
        ESP_LOGI("sensors_task", "DHT11 Reading - Status: %d, Temperature: %f, Humidity: %f\n", dht_err, dht11_temp, dht11_hum);
        dht_err = dht_read_float_data(DHT_TYPE_AM2301, dht22_gpio, &dht22_hum, &dht22_temp);
        ESP_LOGI("sensors_task", "DHT22 Reading - Status: %d, Temperature: %f, Humidity: %f\n", dht_err, dht22_temp, dht22_hum);
        //ds18b20_temp_ = read_roots_temp();
        dht_err = ds18b20_trigger_temperature_conversion(ds18b20s[0]);
        dht_err = ds18b20_get_temperature(ds18b20s[0], &ds18b20_temp_);
        ESP_LOGI("sensors_task", "temperature read from root DS18B20: %.2fC", ds18b20_temp_);

        // update data in global state struct
        if (xSemaphoreTake(device_state_mutex, portMAX_DELAY) == pdTRUE)
        {
            global_device_state.uptime = esp_timer_get_time()/1000000; // Cast to double for int64_t
            global_device_state.ext_temp = dht11_temp;
            global_device_state.ext_hum = dht11_hum;
            global_device_state.int_temp = dht22_temp;
            global_device_state.int_hum = dht22_hum;
            global_device_state.roots_temp = ds18b20_temp_;
            // Release the mutex after accessing the shared data
            xSemaphoreGive(device_state_mutex);
        }
        // Wait for 4 seconds before the next reading
        vTaskDelay(2500 / portTICK_PERIOD_MS);
    }
}




// Initialize mDNS service
void start_mdns_service(void) {
    // Initialize mDNS
    ESP_ERROR_CHECK(mdns_init());

    // Set the hostname for the device
    const char* name = CONFIG_MY_MDNS_NAME;
    ESP_ERROR_CHECK(mdns_hostname_set(name));
    ESP_LOGI("mDNS", "mDNS hostname set to: %s.local", name);

    // Set an optional instance name
    ESP_ERROR_CHECK(mdns_instance_name_set(name));

    // Advertise a service over mDNS (HTTP service on port 80)
    ESP_ERROR_CHECK(mdns_service_add(name, "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI("mDNS", "mDNS service added: http://%s.local/hello", name);
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

    // search and init ds18b20 devices
    init_ds18b20();
    test_read_ds18b20_data();

    // fill device_state with default values
    global_device_state.ch0 = 1;
    global_device_state.ch1 = 1;
    global_device_state.ch2 = 1;
    global_device_state.ch3 = 1;  // they are inverted
    global_device_state.pcb_temp = -255;
    global_device_state.roots_temp = -255;
    global_device_state.ext_temp = -255;
    global_device_state.ext_hum = -255;
    global_device_state.int_temp = -255;
    global_device_state.int_hum = -255;
    global_device_state.uptime = esp_timer_get_time()/1000000; // mks to seconds

    // Create the mutex before using it
    device_state_mutex = xSemaphoreCreateMutex();

    xTaskCreate(&dht_task, "dht_task", 4096, NULL, tskIDLE_PRIORITY, NULL);

    static httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // start mdns server on all default network interfaces
    // https://docs.espressif.com/projects/esp-protocols/mdns/docs/latest/en/index.html
    start_mdns_service();

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
    uint32_t sec;
    uint32_t us;
    sntp_get_system_time(&sec, &us);
    ESP_LOGI(MTAG, "System time: %lu, %lu", sec, us);


    // Start the server for the first time 
    server = start_webserver();


    // start wireguard
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
    ESP_ERROR_CHECK(esp_wireguard_connect(&ctx));
    // wait to really connect to wg server
    while (1) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        err = esp_wireguardif_peer_is_up(&ctx);
        if (err == ESP_OK) {
            ESP_LOGI(MTAG, "Peer is up");
            break;
        } else {
            ESP_LOGI(MTAG, "Peer is down");
        }
    }

    ESP_LOGI(MTAG, "Peer is up !!!!");
    ESP_ERROR_CHECK(esp_wireguard_set_default(&ctx));
    // NOTE
    // for some reason https://github.com/droscy/esp_wireguard contains strange feature - user set mask changed to 255.255.255.255
    // with this param nothing works, so we have to add correct addr with correct mask 255.255.255.0
    // ESP_ERROR_CHECK(esp_wireguard_add_allowed_ip(&ctx, "10.10.0.5", "255.255.255.0"));
    // or change that directly in lib, which i already did


    //time_t last_handshake_time = 0;

    while (server) {
        sleep(10);

        //esp_wireguard_latest_handshake(&ctx, &last_handshake_time);
        //ESP_LOGI(MTAG, "last handshake time %lld", last_handshake_time);
        //ESP_ERROR_CHECK(esp_wireguard_connect(&ctx));
    }
}
