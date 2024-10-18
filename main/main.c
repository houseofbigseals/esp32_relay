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
#include <cJSON.h>
#include "driver/gpio.h"
#include "freertos/task.h"
#include "esp_timer.h"

#define EXAMPLE_HTTP_QUERY_KEY_MAX_LEN  (64)

/* A simple example that demonstrates how to create GET and POST
 * handlers for the web server.
 */

static const char *TAG = "relay_board";
static wireguard_config_t wg_config = ESP_WIREGUARD_CONFIG_DEFAULT();


typedef struct device_state{
    bool ch0;
    bool ch1;
    bool ch2;
    bool ch3;
    float pcb_temp;
    float roots_temp;
    float ext_temp;
    float ext_hum;
    float int_temp;
    float int_hum;
    int64_t uptime;

}device_state;

// Global device state
device_state global_device_state;

// Global mutex for thread safety
SemaphoreHandle_t device_state_mutex;


// Function to create cJSON object from device_state struct with thread safety
cJSON *create_json_from_device_state_safe() {
    // Try to take the mutex before accessing the shared data
    if (xSemaphoreTake(device_state_mutex, portMAX_DELAY) == pdTRUE) {
        // Create the root cJSON object
        cJSON *json = cJSON_CreateObject();
        if (json == NULL) {
            // Release the mutex before returning
            xSemaphoreGive(device_state_mutex);
            return NULL;
        }
        
        // Add fields from the device_state struct to the cJSON object
        cJSON_AddBoolToObject(json, "ch0_pwm", global_device_state.ch0);
        cJSON_AddBoolToObject(json, "ch1_pwm", global_device_state.ch1);
        cJSON_AddBoolToObject(json, "ch2_pwm", global_device_state.ch2);
        cJSON_AddBoolToObject(json, "ch3_pwm", global_device_state.ch3);
        cJSON_AddNumberToObject(json, "pcb_temp", global_device_state.pcb_temp);
        cJSON_AddNumberToObject(json, "roots_temp", global_device_state.roots_temp);
        cJSON_AddNumberToObject(json, "ext_temp", global_device_state.ext_temp);
        cJSON_AddNumberToObject(json, "ext_hum", global_device_state.ext_hum);
        cJSON_AddNumberToObject(json, "int_temp", global_device_state.int_temp);
        cJSON_AddNumberToObject(json, "int_hum", global_device_state.int_hum);
        cJSON_AddNumberToObject(json, "uptime", (double)global_device_state.uptime); // Cast to double for int64_t
        
        // Release the mutex after accessing the shared data
        xSemaphoreGive(device_state_mutex);
        
        // Return the created cJSON object
        return json;
    } else {
        // Failed to take the mutex (unlikely if portMAX_DELAY is used)
        return NULL;
    }
}



/* An HTTP GET handler */
static esp_err_t hello_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;

    /* Get header value string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_hdr_value_len(req, "Host") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        /* Copy null terminated value string into buffer */
        if (httpd_req_get_hdr_value_str(req, "Host", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Host: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-2") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_hdr_value_str(req, "Test-Header-2", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Test-Header-2: %s", buf);
        }
        free(buf);
    }

    buf_len = httpd_req_get_hdr_value_len(req, "Test-Header-1") + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_hdr_value_str(req, "Test-Header-1", buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found header => Test-Header-1: %s", buf);
        }
        free(buf);
    }

    /* Read URL query string length and allocate memory for length + 1,
     * extra byte for null termination */
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = malloc(buf_len);
        ESP_RETURN_ON_FALSE(buf, ESP_ERR_NO_MEM, TAG, "buffer alloc failed");
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            ESP_LOGI(TAG, "Found URL query => %s", buf);
            char param[EXAMPLE_HTTP_QUERY_KEY_MAX_LEN], dec_param[EXAMPLE_HTTP_QUERY_KEY_MAX_LEN] = {0};
            /* Get value of expected key from query string */
            if (httpd_query_key_value(buf, "query1", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query1=%s", param);
                example_uri_decode(dec_param, param, strnlen(param, EXAMPLE_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
            }
            if (httpd_query_key_value(buf, "query3", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query3=%s", param);
                example_uri_decode(dec_param, param, strnlen(param, EXAMPLE_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
            }
            if (httpd_query_key_value(buf, "query2", param, sizeof(param)) == ESP_OK) {
                ESP_LOGI(TAG, "Found URL query parameter => query2=%s", param);
                example_uri_decode(dec_param, param, strnlen(param, EXAMPLE_HTTP_QUERY_KEY_MAX_LEN));
                ESP_LOGI(TAG, "Decoded query parameter => %s", dec_param);
            }
        }
        free(buf);
    }

    /* Set some custom headers */
    httpd_resp_set_hdr(req, "Custom-Header-1", "Custom-Value-1");
    httpd_resp_set_hdr(req, "Custom-Header-2", "Custom-Value-2");

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    const char* resp_str = (const char*) req->user_ctx;
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    /* After sending the HTTP response the old HTTP request
     * headers are lost. Check if HTTP request headers can be read now. */
    if (httpd_req_get_hdr_value_len(req, "Host") == 0) {
        ESP_LOGI(TAG, "Request headers lost");
    }
    return ESP_OK;
}

static const httpd_uri_t hello = {
    .uri       = "/hello",
    .method    = HTTP_GET,
    .handler   = hello_get_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = "Hello World!"
};


/* An HTTP_ANY handler */
static esp_err_t any_handler(httpd_req_t *req)
{
    /* Send response with body set as the
     * string passed in user context*/
    const char* resp_str = (const char*) req->user_ctx;
    httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    // End response
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static const httpd_uri_t any = {
    .uri       = "/any",
    .method    = HTTP_ANY,
    .handler   = any_handler,
    /* Let's pass response string in user
     * context to demonstrate it's usage */
    .user_ctx  = "Hello World!"
};

/* This handler allows the custom error handling functionality to be
 * tested from client side. For that, when a PUT request 0 is sent to
 * URI /ctrl, the /hello and /echo URIs are unregistered and following
 * custom error handler http_404_error_handler() is registered.
 * Afterwards, when /hello or /echo is requested, this custom error
 * handler is invoked which, after sending an error message to client,
 * either closes the underlying socket (when requested URI is /echo)
 * or keeps it open (when requested URI is /hello). This allows the
 * client to infer if the custom error handler is functioning as expected
 * by observing the socket state.
 */
esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err)
{
    if (strcmp("/hello", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/hello URI is not available");
        /* Return ESP_OK to keep underlying socket open */
        return ESP_OK;
    } else if (strcmp("/echo", req->uri) == 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "/echo URI is not available");
        /* Return ESP_FAIL to close underlying socket */
        return ESP_FAIL;
    }
    /* For any other URI send 404 and close socket */
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Some 404 error message");
    return ESP_FAIL;
}


static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &hello);
        httpd_register_uri_handler(server, &any);
        #if CONFIG_EXAMPLE_BASIC_AUTH
        httpd_register_basic_auth(server);
        #endif
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

static esp_err_t stop_webserver(httpd_handle_t server)
{
    // Stop the httpd server
    return httpd_stop(server);
}

static void disconnect_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server) {
        ESP_LOGI(TAG, "Stopping webserver");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAG, "Failed to stop http server");
        }
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAG, "Starting webserver");
        *server = start_webserver();
    }
}


void app_main(void)
{

    // fill device_state with default values
    global_device_state.ch0 = 0;
    global_device_state.ch1 = 0;
    global_device_state.ch2 = 0;
    global_device_state.ch3 = 0;
    global_device_state.pcb_temp = 0;
    global_device_state.roots_temp = 0;
    global_device_state.ext_temp = 0;
    global_device_state.ext_hum = 0;
    global_device_state.int_temp = 0;
    global_device_state.int_hum = 0;
    global_device_state.uptime = esp_timer_get_time()/1000000; // mks to seconds

    // Create the mutex before using it
    device_state_mutex = xSemaphoreCreateMutex();
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
    ESP_LOGI(TAG, "Initializing WireGuard.");
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

    ESP_LOGI(TAG, "Connecting to the peer.");
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
            ESP_LOGI(TAG, "Peer is up");
            break;
        } else {
            ESP_LOGI(TAG, "Peer is down");
        }
    }

    ESP_LOGI(TAG, "Peer is up !!!!");
    ESP_ERROR_CHECK(esp_wireguard_set_default(&ctx));
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
        //ESP_LOGI(TAG, "last handshake time %lld", last_handshake_time);
        //ESP_ERROR_CHECK(esp_wireguard_connect(&ctx));
    }
}
