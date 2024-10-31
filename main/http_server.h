#ifndef GHGFHFGHJHGFGHJ
#define GHGFHFGHJHGFGHJ

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
#include <cJSON.h>
#include "driver/gpio.h"
#include "mdns.h"

/* A simple example that demonstrates how to create GET and POST
 * handlers for the web server.
 */


#define EXAMPLE_HTTP_QUERY_KEY_MAX_LEN  (64)
static const char *TAGG = "http_server";

typedef struct device_state{
    int ch0;
    int ch1;
    int ch2;
    int ch3;
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
cJSON *response_json;

// default relay gpio pins
// set relay gpio params
gpio_num_t relay_0_gpio = 13;
gpio_num_t relay_1_gpio = 12;
gpio_num_t relay_2_gpio = 11;
gpio_num_t relay_3_gpio = 10;


// method to init gpio pin for each relay
void init_relays()
{
    gpio_set_direction(relay_0_gpio, GPIO_MODE_OUTPUT);
    gpio_set_direction(relay_1_gpio, GPIO_MODE_OUTPUT);
    gpio_set_direction(relay_2_gpio, GPIO_MODE_OUTPUT);
    gpio_set_direction(relay_3_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(relay_0_gpio, 1);
    gpio_set_level(relay_1_gpio, 1);
    gpio_set_level(relay_2_gpio, 1);
    gpio_set_level(relay_3_gpio, 1);  // they are inverted by hardware
}

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
        cJSON_AddNumberToObject(json, "ch0", !global_device_state.ch0);
        cJSON_AddNumberToObject(json, "ch1", !global_device_state.ch1);
        cJSON_AddNumberToObject(json, "ch2", !global_device_state.ch2);
        cJSON_AddNumberToObject(json, "ch3", !global_device_state.ch3);
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
static esp_err_t info_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;

    /* Set some custom headers */
    httpd_resp_set_hdr(req, "Custom-Header-1", "Custom-Value-1");
    httpd_resp_set_hdr(req, "Custom-Header-2", "Custom-Value-2");

    /* Send response with custom headers and body set as the
     * string passed in user context*/
    //const char* resp_str = "Hello World!" ; //(const char*) req->user_ctx;
    //httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);

    // calculate current time
    //int64_t current_time = esp_timer_get_time()/1000000;  // time from last boot in microseconds -> seconds

    // update time in global state struct
    // for now we cannot read data from second sensor
    if (xSemaphoreTake(device_state_mutex, portMAX_DELAY) == pdTRUE) {
        global_device_state.uptime = esp_timer_get_time()/1000000; // Cast to double for int64_t
        // Release the mutex after accessing the shared data
        xSemaphoreGive(device_state_mutex);
    }
    
    // Create JSON from device state safely
    response_json = create_json_from_device_state_safe();

    // Convert cJSON object to string and print it
    if (response_json != NULL) {
        char *json_string = cJSON_Print(response_json);
        cJSON_Delete(response_json);
        //ESP_LOGI(TAG,"current device state was requested:\n %s\n", json_string);

        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, json_string, strlen(json_string));

        // Cleanup
        free(json_string);

    } else {
        ESP_LOGI(TAGG,"Failed to create JSON!\n");
    }

    /* After sending the HTTP response the old HTTP request
     * headers are lost. Check if HTTP request headers can be read now. */
    if (httpd_req_get_hdr_value_len(req, "Host") == 0) {
        ESP_LOGI(TAGG, "Request headers lost");
    }
    return ESP_OK;
}

static const httpd_uri_t info = {
    .uri       = "/info",
    .method    = HTTP_GET,
    .handler   = info_get_handler,
    //Let's pass response string in user
    // context to demonstrate it's usage 
    .user_ctx  = NULL  // "Hello World!"
};

// An HTTP GET handler for ext temp data
static esp_err_t ext_temp_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;
    float res = -255;

    // update time in global state struct
    // for now we cannot read data from second sensor
    if (xSemaphoreTake(device_state_mutex, portMAX_DELAY) == pdTRUE) {
        global_device_state.uptime = esp_timer_get_time()/1000000; // Cast to double for int64_t
        res = global_device_state.ext_temp;
        // Release the mutex after accessing the shared data
        xSemaphoreGive(device_state_mutex);
    }
    // convert to string
    int len = snprintf(NULL, 0, "%f", res);
    char *result = malloc(len + 1);
    snprintf(result, len + 1, "%f", res);
    ESP_LOGI(TAGG, "Requested ext_temp: %s", result);

    // send it back
    httpd_resp_send(req, result, HTTPD_RESP_USE_STRLEN);
    free(result);
    return ESP_OK;
}

static const httpd_uri_t ext_temp = {
    .uri       = "/ext_temp",
    .method    = HTTP_GET,
    .handler   = ext_temp_get_handler,
    .user_ctx  = NULL
};


// An HTTP GET handler for ext hum data 
static esp_err_t ext_hum_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;
    float res = -255;

    // update time in global state struct
    // for now we cannot read data from second sensor
    if (xSemaphoreTake(device_state_mutex, portMAX_DELAY) == pdTRUE) {
        global_device_state.uptime = esp_timer_get_time()/1000000; // Cast to double for int64_t
        res = global_device_state.ext_hum;
        // Release the mutex after accessing the shared data
        xSemaphoreGive(device_state_mutex);
    }
    // convert to string
    int len = snprintf(NULL, 0, "%f", res);
    char *result = malloc(len + 1);
    snprintf(result, len + 1, "%f", res);
    ESP_LOGI(TAGG, "Requested ext_hum: %s", result);

    // send it back
    httpd_resp_send(req, result, HTTPD_RESP_USE_STRLEN);
    free(result);
    return ESP_OK;
}

static const httpd_uri_t ext_hum = {
    .uri       = "/ext_hum",
    .method    = HTTP_GET,
    .handler   = ext_hum_get_handler,
    .user_ctx  = NULL  
};

// An HTTP GET handler for int hum data 
static esp_err_t int_hum_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;
    float res = -255;

    // update time in global state struct
    // for now we cannot read data from second sensor
    if (xSemaphoreTake(device_state_mutex, portMAX_DELAY) == pdTRUE) {
        global_device_state.uptime = esp_timer_get_time()/1000000; // Cast to double for int64_t
        res = global_device_state.int_hum;
        // Release the mutex after accessing the shared data
        xSemaphoreGive(device_state_mutex);
    }
    // convert to string
    int len = snprintf(NULL, 0, "%f", res);
    char *result = malloc(len + 1);
    snprintf(result, len + 1, "%f", res);
    ESP_LOGI(TAGG, "Requested int_hum: %s", result);

    // send it back
    httpd_resp_send(req, result, HTTPD_RESP_USE_STRLEN);
    free(result);
    return ESP_OK;
}

static const httpd_uri_t int_hum = {
    .uri       = "/int_hum",
    .method    = HTTP_GET,
    .handler   = int_hum_get_handler,
    .user_ctx  = NULL
};

// An HTTP GET handler for int temp data 
static esp_err_t int_temp_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;
    float res = -255;

    // update time in global state struct
    // for now we cannot read data from second sensor
    if (xSemaphoreTake(device_state_mutex, portMAX_DELAY) == pdTRUE) {
        global_device_state.uptime = esp_timer_get_time()/1000000; // Cast to double for int64_t
        res = global_device_state.int_temp;
        // Release the mutex after accessing the shared data
        xSemaphoreGive(device_state_mutex);
    }
    // convert to string
    int len = snprintf(NULL, 0, "%f", res);
    char *result = malloc(len + 1);
    snprintf(result, len + 1, "%f", res);
    ESP_LOGI(TAGG, "Requested int_temp: %s", result);

    // send it back
    httpd_resp_send(req, result, HTTPD_RESP_USE_STRLEN);
    free(result);
    return ESP_OK;
}

static const httpd_uri_t int_temp = {
    .uri       = "/int_temp",
    .method    = HTTP_GET,
    .handler   = int_temp_get_handler,
    .user_ctx  = NULL
};

// An HTTP GET handler for roots temp data 
static esp_err_t roots_temp_get_handler(httpd_req_t *req)
{
    char*  buf;
    size_t buf_len;
    float res = -255;

    // update time in global state struct
    // for now we cannot read data from second sensor
    if (xSemaphoreTake(device_state_mutex, portMAX_DELAY) == pdTRUE) {
        global_device_state.uptime = esp_timer_get_time()/1000000; // Cast to double for int64_t
        res = global_device_state.roots_temp;
        // Release the mutex after accessing the shared data
        xSemaphoreGive(device_state_mutex);
    }
    // convert to string
    int len = snprintf(NULL, 0, "%f", res);
    char *result = malloc(len + 1);
    snprintf(result, len + 1, "%f", res);
    ESP_LOGI(TAGG, "Requested roots_temp: %s", result);

    // send it back
    httpd_resp_send(req, result, HTTPD_RESP_USE_STRLEN);
    free(result);
    return ESP_OK;
}


static const httpd_uri_t roots_temp = {
    .uri       = "/roots_temp",
    .method    = HTTP_GET,
    .handler   = roots_temp_get_handler,
    .user_ctx  = NULL
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

// device can be controlled via POST request like that
// curl -X POST http://10.10.0.5/relay -H 'Content-Type: application/json' -d '{"channel":3,"state": 1}'
static esp_err_t relay_post_handler(httpd_req_t *req)
{
    char buf[100];
    int ret = 0;
    int remaining = req->content_len;

    // Read the data from the request
    while (remaining > 0) {
        // This will copy the request body into the buffer
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                // Retry receiving if timeout occurred
                continue;
            }
            return ESP_FAIL;
        }
        remaining -= ret;
    }

    // Null-terminate the buffer
    buf[ret] = '\0';
    ESP_LOGI(TAGG, "Received: %s", buf);

    // Parse the JSON data
    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        ESP_LOGE(TAGG, "Failed to parse JSON");
        char* resp = "RESULT: ERROR Failed to parse JSON\n";
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_FAIL;
    }

    // Extract the "led_num" value
    cJSON *channel = cJSON_GetObjectItem(json, "channel");
    if (!cJSON_IsNumber(channel)) {
        ESP_LOGE(TAGG, "Invalid channel");
        char* resp = "RESULT: ERROR Invalid channel type\n";
        httpd_resp_send(req, resp, strlen(resp));
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    // Extract the "state" value
    cJSON *state = cJSON_GetObjectItem(json, "state");
    if (!cJSON_IsNumber(state)) {
        ESP_LOGE(TAGG, "Invalid state");
        char* resp = "RESULT: ERROR Invalid state type\n";
        httpd_resp_send(req, resp, strlen(resp));
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    // Get the values
    uint channel_value = channel->valueint;
    uint state_value = state->valueint;

    ESP_LOGI(TAGG, "Requested update - relay channel: %d, state: %d", channel_value, state_value);

    // Work with the values - for now just here
    // in future - using queue
    int err_code = 0;
    if(((state_value == 0) || (state_value == 1)) && (channel_value < 4))
    {
        switch(channel_value)
        {
        case 0:
            gpio_set_level(relay_0_gpio, !state_value); // they are inverted
            ESP_LOGI(TAGG, "Setting relay %d in state %d", channel_value, (bool)!state_value);
            break;
        case 1:
            gpio_set_level(relay_1_gpio, !state_value); // they are inverted
            ESP_LOGI(TAGG, "Setting relay %d in state %d", channel_value, (bool)!state_value);
            break;
        case 2:
            gpio_set_level(relay_2_gpio, !state_value); // they are inverted
            ESP_LOGI(TAGG, "Setting relay %d in state %d", channel_value, (bool)!state_value);
            break;
        case 3:
            gpio_set_level(relay_3_gpio, !state_value); // they are inverted
            ESP_LOGI(TAGG, "Setting relay %d in state %d", channel_value, (bool)!state_value);
            break;
        default:
            ESP_LOGI(TAGG, "No such relay!");
        }
        

        // update global state struct
        if (xSemaphoreTake(device_state_mutex, portMAX_DELAY) == pdTRUE) 
        {
            global_device_state.uptime = esp_timer_get_time()/1000000; // mks to s

            switch(channel_value)
            {
            case 0:
                global_device_state.ch0 = (bool)!state_value; 
                break;
            case 1:
                global_device_state.ch1 = (bool)!state_value;
                break;
            case 2:
                global_device_state.ch2 = (bool)!state_value;
                break;
            case 3:
                global_device_state.ch3 = (bool)!state_value;
                break;
            }
            // Release the mutex after accessing the shared data
            xSemaphoreGive(device_state_mutex);
        }
        char resp[60];  // Create a buffer to hold the formatted string
        snprintf(resp, sizeof(resp), "RESULT: SUCCESS\n Relay: %d, set to state: %d\n", channel_value, state_value);
        // Send a response
        httpd_resp_send(req, resp, strlen(resp));
    }
    else
    {
        char* resp = "RESULT: ERROR invalid params!\n";
        httpd_resp_send(req, resp, strlen(resp));
    }

    // Clean up
    cJSON_Delete(json);

    return ESP_OK;
}


static const httpd_uri_t relay = {
    .uri       = "/relay",
    .method    = HTTP_POST,
    .handler   = relay_post_handler,
    .user_ctx  = NULL
};

// curl -X POST http://esp32_relay_1.local/reset -H 'Content-Type: text/html' -d 'force_reset'
static esp_err_t reset_post_handler(httpd_req_t *req)
{
    char buf[100];
    int ret = 0;
    int remaining = req->content_len;

    // Read the data from the request
    while (remaining > 0) {
        // This will copy the request body into the buffer
        if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                // Retry receiving if timeout occurred
                continue;
            }
            return ESP_FAIL;
        }
        remaining -= ret;
    }

    // Null-terminate the buffer
    buf[ret] = '\0';
    ESP_LOGI(TAGG, "Received: %s", buf);

    // check if it is real desire to reset
    const char* reset_command = "force_reset";
    if (strcmp(buf, reset_command) == 0)
    {
        ESP_LOGI(TAGG, "User have sent force reset command, so rebooting");
        char* resp = "User have sent force reset command, so rebooting\n";
        httpd_resp_send(req, resp, strlen(resp));
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        // make reset
        esp_restart();
        return ESP_OK;
    }
    else
    {
        ESP_LOGI(TAGG, "User have sent incorrect reset command, so no rebooting");
        char* resp = "User have sent incorrect reset command, so no rebooting\n";
        httpd_resp_send(req, resp, strlen(resp));
        return ESP_OK;
    }
}

static const httpd_uri_t reset = {
    .uri       = "/reset",
    .method    = HTTP_POST,
    .handler   = reset_post_handler,
    .user_ctx  = NULL
};


static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    // Start the httpd server
    ESP_LOGI(TAGG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAGG, "Registering URI handlers");
        httpd_register_uri_handler(server, &info);
        httpd_register_uri_handler(server, &relay);
        httpd_register_uri_handler(server, &ext_temp);
        httpd_register_uri_handler(server, &int_temp); 
        httpd_register_uri_handler(server, &ext_hum);
        httpd_register_uri_handler(server, &int_hum);
        httpd_register_uri_handler(server, &roots_temp);
        httpd_register_uri_handler(server, &reset);
        httpd_register_uri_handler(server, &any);
        #if CONFIG_EXAMPLE_BASIC_AUTH
        httpd_register_basic_auth(server);
        #endif
        return server;
    }

    ESP_LOGI(TAGG, "Error starting server!");
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
        ESP_LOGI(TAGG, "Stopping webserver");
        if (stop_webserver(*server) == ESP_OK) {
            *server = NULL;
        } else {
            ESP_LOGE(TAGG, "Failed to stop http server");
        }
    }
}

static void connect_handler(void* arg, esp_event_base_t event_base,
                            int32_t event_id, void* event_data)
{
    httpd_handle_t* server = (httpd_handle_t*) arg;
    if (*server == NULL) {
        ESP_LOGI(TAGG, "Starting webserver");
        *server = start_webserver();
    }
}


#endif