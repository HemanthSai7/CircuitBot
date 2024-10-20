// #include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include <esp_http_server.h>
#include "esp_spiffs.h" // Include SPIFFS header
#include "cJSON.h"      // Include cJSON for JSON parsing
#include "ir_nec_encoder.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "ir_nec_encoder.h"
#include "mdns.h"
#include "driver/uart.h"
#include <string.h>
static uint8_t led_state = 0; // off
#include "esp_spiffs.h"

#include <stdio.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "cJSON.h"

#define UART_NUM UART_NUM_2 // Use UART2
#define TX_PIN 21           // Transmit Pin
#define RX_PIN 22           // Receive Pin
#define BUF_SIZE (1024)

static const char *TAG = "Circuit_Bot";
static QueueHandle_t uart_queue;

#define NEC_LEADING_CODE_DURATION_0 9000
#define NEC_LEADING_CODE_DURATION_1 4500
#define NEC_PAYLOAD_ZERO_DURATION_0 560
#define NEC_PAYLOAD_ZERO_DURATION_1 560
#define NEC_PAYLOAD_ONE_DURATION_0 560
#define NEC_PAYLOAD_ONE_DURATION_1 1690
#define NEC_REPEAT_CODE_DURATION_0 9000
#define NEC_REPEAT_CODE_DURATION_1 2250

#define WIFI_SSID "Circuit_Bot"
#define WIFI_PASSWORD "12345678"
#define WIFI_CHANNEL 1
#define WIFI_MAX_STA 4

#define LED_PIN GPIO_NUM_32

// static rmt_channel_handle_t tx_channel = NULL;
static rmt_encoder_handle_t nec_encoder = NULL;

#define EXAMPLE_IR_RESOLUTION_HZ 1000000 // 1MHz resolution, 1 tick = 1us
#define EXAMPLE_IR_TX_GPIO_NUM 18
#define EXAMPLE_IR_RX_GPIO_NUM 19
#define EXAMPLE_IR_NEC_DECODE_MARGIN 200 // Tolerance for parsing RMT symbols into bit stream

void ir_send_command(uint16_t address, uint16_t command);

static uint16_t s_nec_code_address;
static uint16_t s_nec_code_command;

static int received_data = 0;
static int received_data_1 = 0;

static int receive_flag = 0;
static int search_flag = 0;

rmt_tx_channel_config_t tx_channel_cfg = {
    .clk_src = RMT_CLK_SRC_DEFAULT,
    .resolution_hz = EXAMPLE_IR_RESOLUTION_HZ,
    .mem_block_symbols = 64, // amount of RMT symbols that the channel can store at a time
    .trans_queue_depth = 4,  // number of transactions that allowed to pending in the background, this example won't queue multiple transactions, so queue depth > 1 is sufficient
    .gpio_num = EXAMPLE_IR_TX_GPIO_NUM,
};

#define UART_BUFFER_SIZE 256
void send_json_file_content(const char *file_path)
{
    FILE *f = fopen(file_path, "r");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path);
        return;
    }

    // Get the file size
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Allocate memory for the file content
    char *file_content = malloc(file_size + 1);
    if (!file_content)
    {
        ESP_LOGE(TAG, "Memory allocation failed");
        fclose(f);
        return;
    }

    // Read the file content
    size_t read_size = fread(file_content, 1, file_size, f);
    fclose(f);
    if (read_size != file_size)
    {
        ESP_LOGE(TAG, "Failed to read the complete file");
        free(file_content);
        return;
    }
    file_content[file_size] = '\0'; // Null-terminate

    ESP_LOGI(TAG, "DATA TRANSMITTING STARTED");

    // Send the content over UART in chunks
    size_t bytes_sent = 0;
    while (bytes_sent < file_size)
    {
        size_t chunk_size = (file_size - bytes_sent) > UART_BUFFER_SIZE ? UART_BUFFER_SIZE : (file_size - bytes_sent);

        int sent = uart_write_bytes(UART_NUM, file_content + bytes_sent, chunk_size);
        if (sent < 0)
        {
            ESP_LOGE(TAG, "UART write failed");
            free(file_content);
            return;
        }

        bytes_sent += sent;
        vTaskDelay(pdMS_TO_TICKS(100)); // Small delay between chunks
    }

    ESP_LOGI(TAG, "Data transmission completed");
    free(file_content);
}

// UART Event Task to handle incoming data
static void uart_event_task(void *pvParameters)
{
    uart_event_t event;
    uint8_t data[BUF_SIZE];
    uint16_t address = 0, command = 0;

    while (1)
    {
        // Wait for UART events
        if (xQueueReceive(uart_queue, (void *)&event, portMAX_DELAY))
        {
            if (event.type == UART_DATA)
            {
                int length = uart_read_bytes(UART_NUM, data, event.size, portMAX_DELAY);
                data[length] = '\0'; // Null-terminate the received string
                ESP_LOGI(TAG, "The data %s", data);
                if (strcmp((char *)data, "get_details") == 0)
                {
                    ESP_LOGI(TAG, "STARTED SENDING");
                    send_json_file_content("/spiffs/devices.json");
                }
                else if (strncmp((char *)data, "data{", 5) == 0)
                {
                    // Find the address and command
                    char *address_ptr = strstr((char *)data, "address=");
                    char *command_ptr = strstr((char *)data, "command=");

                    if (address_ptr && command_ptr)
                    {
                        // Extract address
                        address_ptr += strlen("address=");                 // Move pointer to the start of the value
                        char address_str[6];                               // Buffer to hold the address (5 hex digits + null terminator)
                        sscanf(address_ptr, "%5[0-9A-Fa-f]", address_str); // Read up to 5 hex digits
                        address = (uint32_t)strtol(address_str, NULL, 16); // Convert to uint16_t

                        // Extract command
                        command_ptr += strlen("command=");                 // Move pointer to the start of the value
                        char command_str[6];                               // Buffer to hold the command (5 hex digits + null terminator)
                        sscanf(command_ptr, "%5[0-9A-Fa-f]", command_str); // Read up to 5 hex digits
                        command = (uint32_t)strtol(command_str, NULL, 16); // Convert to uint16_t

                        ESP_LOGI(TAG, "Parsed Address: 0x%05X, Command: 0x%05X", address, command);
                        ir_send_command(address, command); // Send the parsed values
                    }
                    else
                    {
                        ESP_LOGE(TAG, "Address or Command not found in: %s", data);
                    }
                }
                else
                {
                    ESP_LOGE(TAG, "Invalid format: %s", data);
                }
            }
        }
    }
}

// Initialize UART with interrupt-driven handling
void init_uart(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, BUF_SIZE * 4, BUF_SIZE * 4, 20, &uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    // ESP_ERROR_CHECK(uart_set_pin(UART_NUM, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, TX_PIN, RX_PIN, NULL, NULL));
    // Create a task to handle UART events
    xTaskCreate(uart_event_task, "uart_event_task", 4096, NULL, 10, NULL);
}

// rmt_encoder_handle_t nec_encoder = NULL;
void setup_mdns(void)
{
    ESP_ERROR_CHECK(mdns_init());
    // Set the hostname to "circuitbot.com"
    ESP_ERROR_CHECK(mdns_hostname_set("circuitbot"));
    ESP_LOGI(TAG, "Hostname set to: circuitbot.local");

    // Optional: Set an instance name (for UI discovery)
    ESP_ERROR_CHECK(mdns_instance_name_set("CircuitBot Device"));

    // Advertise HTTP service on port 80
    ESP_ERROR_CHECK(mdns_service_add("Web Server", "_http", "_tcp", 80, NULL, 0));
    ESP_LOGI(TAG, "mDNS service added: Web Server on circuitbot.local:80");
}

/**
 * @brief Sends an NEC command using the IR transmitter
 *
 * @param address The 16-bit address for the NEC protocol
 * @param command The 16-bit command to transmit
 */
void ir_send_command(uint16_t address, uint16_t command)
{
    // the following timing requirement is based on NEC protocol
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 1250,     // the shortest duration for NEC signal is 560us, 1250ns < 560us, valid signal won't be treated as noise
        .signal_range_max_ns = 12000000, // the longest duration for NEC signal is 9000us, 12000000ns > 9000us, the receive won't stop early
    };

    ESP_LOGI(TAG, "create RMT TX channel");

    rmt_channel_handle_t tx_channel = NULL;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_channel_cfg, &tx_channel));

    ESP_LOGI(TAG, "modulate carrier to TX channel");
    rmt_carrier_config_t carrier_cfg = {
        .duty_cycle = 0.33,
        .frequency_hz = 38000, // 38KHz
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_cfg));

    // this example won't send NEC frames in a loop
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0, // no loop
    };

    ESP_LOGI(TAG, "install IR NEC encoder");
    ir_nec_encoder_config_t nec_encoder_cfg = {
        .resolution = EXAMPLE_IR_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_ir_nec_encoder(&nec_encoder_cfg, &nec_encoder));

    ESP_LOGI(TAG, "enable RMT TX and RX channels");
    ESP_ERROR_CHECK(rmt_enable(tx_channel));

    ESP_LOGI(TAG, "Send command, address: 0x%04X, command: 0x%04X", address, command);

    uint32_t scan_code = (address & 0xFFFF) | (command << 16);

    ESP_ERROR_CHECK(rmt_transmit(tx_channel, nec_encoder, &scan_code, sizeof(scan_code), &transmit_config));
    ESP_ERROR_CHECK(rmt_tx_wait_all_done(tx_channel, portMAX_DELAY));
    ESP_LOGI(TAG, "Done transmission");
}
static inline bool nec_check_in_range(uint32_t signal_duration, uint32_t spec_duration)
{
    return (signal_duration < (spec_duration + EXAMPLE_IR_NEC_DECODE_MARGIN)) &&
           (signal_duration > (spec_duration - EXAMPLE_IR_NEC_DECODE_MARGIN));
}

static bool nec_parse_logic0(rmt_symbol_word_t *rmt_nec_symbols)
{
    return nec_check_in_range(rmt_nec_symbols->duration0, NEC_PAYLOAD_ZERO_DURATION_0) &&
           nec_check_in_range(rmt_nec_symbols->duration1, NEC_PAYLOAD_ZERO_DURATION_1);
}

static bool nec_parse_logic1(rmt_symbol_word_t *rmt_nec_symbols)
{
    return nec_check_in_range(rmt_nec_symbols->duration0, NEC_PAYLOAD_ONE_DURATION_0) &&
           nec_check_in_range(rmt_nec_symbols->duration1, NEC_PAYLOAD_ONE_DURATION_1);
}

static bool nec_parse_frame(rmt_symbol_word_t *rmt_nec_symbols)
{
    rmt_symbol_word_t *cur = rmt_nec_symbols;
    uint16_t address = 0;
    uint16_t command = 0;

    if (!nec_check_in_range(cur->duration0, NEC_LEADING_CODE_DURATION_0) ||
        !nec_check_in_range(cur->duration1, NEC_LEADING_CODE_DURATION_1))
    {
        return false;
    }
    cur++;

    for (int i = 0; i < 16; i++, cur++)
    {
        if (nec_parse_logic1(cur))
            address |= 1 << i;
        else if (!nec_parse_logic0(cur))
            return false;
    }

    for (int i = 0; i < 16; i++, cur++)
    {
        if (nec_parse_logic1(cur))
            command |= 1 << i;
        else if (!nec_parse_logic0(cur))
            return false;
    }

    s_nec_code_address = address;
    s_nec_code_command = command;
    return true;
}
static void example_parse_nec_frame(rmt_symbol_word_t *rmt_nec_symbols, size_t symbol_num)
{
    if (symbol_num == 34 && nec_parse_frame(rmt_nec_symbols))
    {
        printf("Address=%04X, Command=%04X\n", s_nec_code_address, s_nec_code_command);

        if (search_flag == 1)
        {
            received_data = s_nec_code_command;
            received_data_1 = s_nec_code_address;
            receive_flag = 1;
            ESP_LOGI(TAG, "The  data received and search flag enabled and made receive flag as 1");
        }
        else
        {
            ESP_LOGI(TAG, "Search Flag not enabled");
        }
    }
}
static bool example_rmt_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata, void *user_data)
{
    BaseType_t high_task_wakeup = pdFALSE;
    QueueHandle_t receive_queue = (QueueHandle_t)user_data;
    xQueueSendFromISR(receive_queue, edata, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

extern const char g_web_page[] asm("_binary_web_page_htm_start");
extern const char g_web_page_2[] asm("_binary_web_page_2_htm_start");
extern const char g_add_device[] asm("_binary_add_device_htm_start");

extern const char g_control[] asm("_binary_control_device_htm_start");

extern const uint8_t icon_png_start[] asm("_binary_icon_png_start");
extern const uint8_t icon_png_end[] asm("_binary_icon_png_end");

// Function to read device details from the JSON file
esp_err_t get_device_details_handler(httpd_req_t *req)
{
    // Open the file for reading
    FILE *f = fopen("/spiffs/devices.json", "r");
    if (!f)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", "/spiffs/devices.json"); // Fixed file path in log
        const char *response = "{\"status\":\"error\", \"message\":\"File not found\"}";
        httpd_resp_send(req, response, strlen(response));
        return ESP_FAIL;
    }

    // Read the existing JSON data
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    // Allocate memory for file content
    char *file_content = malloc(file_size + 1);
    if (!file_content)
    { // Check if memory allocation was successful
        ESP_LOGE(TAG, "Memory allocation failed");
        fclose(f);
        const char *response = "{\"status\":\"error\", \"message\":\"Memory allocation failed\"}";
        httpd_resp_send(req, response, strlen(response));
        return ESP_FAIL;
    }

    // Read the file content
    size_t read_size = fread(file_content, 1, file_size, f);
    fclose(f);
    if (read_size != file_size)
    { // Check if read was successful
        ESP_LOGE(TAG, "Failed to read the complete file");
        free(file_content);
        const char *response = "{\"status\":\"error\", \"message\":\"Failed to read file\"}";
        httpd_resp_send(req, response, strlen(response));
        return ESP_FAIL;
    }

    file_content[file_size] = '\0'; // Null-terminate the string

    // Send the response with the JSON data
    httpd_resp_set_type(req, "application/json"); // Set content type to JSON
    httpd_resp_send(req, file_content, file_size);

    // Free allocated memory
    free(file_content);
    return ESP_OK;
}

void print_json_file_content(const char *file_path)
{
    // Open the JSON file for reading
    FILE *file = fopen(file_path, "r");
    if (!file)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", file_path);
        return;
    }

    // Get the file size
    fseek(file, 0, SEEK_END);
    size_t file_size = ftell(file);
    rewind(file);

    // Allocate memory to hold the file content
    char *json_content = malloc(file_size + 1);
    if (!json_content)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for JSON content");
        fclose(file);
        return;
    }

    // Read the file content into the buffer
    fread(json_content, 1, file_size, file);
    json_content[file_size] = '\0';
    fclose(file);

    // Parse the JSON content
    cJSON *root = cJSON_Parse(json_content);
    if (!root)
    {
        ESP_LOGE(TAG, "Failed to parse JSON content");
        free(json_content);
        return;
    }

    // Print the parsed JSON content to the serial log
    char *printed_json = cJSON_Print(root);
    if (printed_json)
    {
        ESP_LOGI(TAG, "JSON Content:\n%s", printed_json);
        free(printed_json);
    }

    // Clean up
    cJSON_Delete(root);
    free(json_content);
}

void init_spiffs()
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = NULL,
        .max_files = 5,
        .format_if_mount_failed = true // Ensures SPIFFS is formatted if unmounted
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);

    if (ret != ESP_OK)
    {
        if (ret == ESP_FAIL)
        {
            ESP_LOGE("SPIFFS", "Failed to mount or format filesystem");
        }
        else if (ret == ESP_ERR_NOT_FOUND)
        {
            ESP_LOGE("SPIFFS", "Partition not found");
        }
        else
        {
            ESP_LOGE("SPIFFS", "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info(NULL, &total, &used);
    if (ret == ESP_OK)
    {
        ESP_LOGI("SPIFFS", "Partition size: total: %d, used: %d", total, used);
    }
    else
    {
        ESP_LOGE("SPIFFS", "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    }
}

// Function to check if name is in NVS and retrieve it
char *get_stored_name()
{
    static char name[32]; // Allocate space for name
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK)
    {
        size_t required_size;
        err = nvs_get_str(nvs_handle, "user_name", NULL, &required_size); // Check size
        if (err == ESP_OK && required_size < sizeof(name))
        {
            nvs_get_str(nvs_handle, "user_name", name, &required_size); // Retrieve name
            nvs_close(nvs_handle);
            ESP_LOGI(TAG, "Retrieved name: %s", name);
            return name;
        }
        nvs_close(nvs_handle);
    }
    return NULL; // Return NULL if no name found or error occurs
}

esp_err_t get_handler(httpd_req_t *req)
{
    char *stored_name = get_stored_name(); // Check if name is stored

    if (stored_name)
    {

        httpd_resp_send(req, g_web_page_2, strlen(g_web_page_2));
    }
    else
    {
        ESP_LOGI(TAG, "SHOWING HOME PAGE FOR NAME");
        httpd_resp_send(req, g_web_page, strlen(g_web_page));
    }
    return ESP_OK;
}

esp_err_t wifi_cred_handler(httpd_req_t *l_req)
{
    // Sending Web page to local server as response
    ESP_LOGI(TAG, "Wifi");
    httpd_resp_send(l_req, g_web_page, strlen(g_web_page));
    return ESP_OK;
}
esp_err_t control_device_handler(httpd_req_t *l_req)
{
    // Sending Web page to local server as response
    ESP_LOGI(TAG, "Wifi");
    httpd_resp_send(l_req, g_control, strlen(g_control));
    return ESP_OK;
}

esp_err_t add_device_handler(httpd_req_t *l_req)
{
    ESP_LOGI(TAG, "Add device");
    httpd_resp_send(l_req, g_add_device, strlen(g_add_device));
    return ESP_OK;
}

esp_err_t on_handler(httpd_req_t *req)
{
    //   gpio_set_level(LED_PIN, 1);
    led_state = 1;
    const char resp[] = "<h3>LED State: ON</h3><a href=\"/on\"><button>Turn ON</button></a><a href=\"/off\"><button>Turn OFF</button</a>";
    ESP_LOGI(TAG, "ON");

    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t off_handler(httpd_req_t *req)
{
    // gpio_set_level(LED_PIN, 0);
    led_state = 0;
    ESP_LOGI(TAG, "Off");
    const char resp[] = "<h3>LED State: OFF</h3><a href=\"/on\"><button>Turn ON</button></a><a href=\"/off\"><button>Turn OFF</button</a>";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Handler to serve the icon
esp_err_t icon_handler(httpd_req_t *req)
{
    // Calculate the size of the icon
    size_t icon_size = icon_png_end - icon_png_start;
    // Set content type for PNG
    httpd_resp_set_type(req, "image/png");
    // Send the icon binary data
    httpd_resp_send(req, (const char *)icon_png_start, icon_size);
    return ESP_OK;
}

esp_err_t submit_add_device_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "SUBMIT ADD DEVICE URI HIT");

    // Receive the request data
    char buf[100]; // Use char instead of int
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0)
    {
        ESP_LOGI(TAG, "ESP SUBMISSION: FAILED");
        return ESP_FAIL;
    }
    buf[ret] = '\0'; // Null-terminate the received data

    cJSON *data = cJSON_Parse(buf);
    if (data == NULL)
    {
        return ESP_FAIL; // Handle JSON parse error
    }

    const cJSON *deviceAction = cJSON_GetObjectItem(data, "deviceAction");
    const cJSON *deviceName = cJSON_GetObjectItem(data, "deviceName");

    if (cJSON_IsString(deviceAction) && cJSON_IsString(deviceName))
    {
        ESP_LOGI(TAG, "Device Name: %s", deviceName->valuestring);
        ESP_LOGI(TAG, "Device Action: %s", deviceAction->valuestring);
        ESP_LOGI(TAG, "Device Command: %x", received_data);
        ESP_LOGI(TAG, "Device Address: %x", received_data_1);

        // Prepare the JSON entry to append
        cJSON *new_entry = cJSON_CreateObject();
        cJSON_AddStringToObject(new_entry, "deviceName", deviceName->valuestring);
        cJSON_AddStringToObject(new_entry, "deviceAction", deviceAction->valuestring);
        cJSON_AddNumberToObject(new_entry, "deviceCommand", received_data);   // Add the 32-bit integer
        cJSON_AddNumberToObject(new_entry, "deviceAddress", received_data_1); // Add the 32-bit integer

        // Open the file for reading
        FILE *f = fopen("/spiffs/devices.json", "r");
        cJSON *root;
        if (f)
        {
            // Read the existing JSON data
            fseek(f, 0, SEEK_END);
            long file_size = ftell(f);
            fseek(f, 0, SEEK_SET);
            char *file_content = malloc(file_size + 1);
            fread(file_content, 1, file_size, f);
            fclose(f);
            file_content[file_size] = '\0'; // Null-terminate

            // Parse existing data
            root = cJSON_Parse(file_content);
            free(file_content);
        }
        else
        {
            // If the file doesn't exist, create a new root object
            root = cJSON_CreateArray();
        }

        // Append the new entry to the root
        cJSON_AddItemToArray(root, new_entry);

        // Write the updated JSON back to the file
        f = fopen("/spiffs/devices.json", "w");
        if (f)
        {
            char *updated_content = cJSON_Print(root);
            fwrite(updated_content, 1, strlen(updated_content), f);
            fclose(f);
            free(updated_content);
        }
        print_json_file_content("/spiffs/devices.json");

        cJSON_Delete(root); // Clean up the JSON object
    }

    cJSON_Delete(data); // Clean up the parsed data
    const char *response = "{\"status\":\"success\"}";
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

esp_err_t submit_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "SUBMIT URI HIT");
    int buf[100];
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0)
    {
        ESP_LOGI(TAG, "ESP SUBMISSION: FAILED");
        return ESP_FAIL;
    }
    cJSON *data = cJSON_Parse(buf);
    if (data == NULL)
    {
        return ESP_FAIL; // Handle JSON parse error
    }

    const cJSON *name = cJSON_GetObjectItem(data, "wifiname");
    if (name && cJSON_IsString(name))
    {
        // Store the name in NVS
        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK)
        {
            nvs_set_str(nvs_handle, "user_name", name->valuestring);
            ESP_LOGI(TAG, "Name:%s", name->valuestring);
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }
    }
    cJSON_Delete(data);
    const char *response = "{\"status\":\"success\"}";
    httpd_resp_send(req, response, strlen(response));
    return ESP_OK;
}

// Simulate device search result
int search_for_device()
{
    // Simulating a device search. Returns 1 on success, 0 on failure.
    search_flag = 1;
    for (int i = 0; i < 30; i++)
    {
        if (receive_flag == 1)
        {
            int data = received_data;
            search_flag = 0;
            ESP_LOGI(TAG, "The data received and stoping searching: %d ", data);
            receive_flag = 0;
            return data;
            // ir_send_command(0x0440, 0x3003);
        }
        vTaskDelay(pdMS_TO_TICKS(100)); // Simulate some delay
    }
    return -1; // Random success or failure
}

// Function to handle device control actions
esp_err_t control_action_uri_handler(httpd_req_t *req)
{
    char buf[100]; // Buffer for incoming data
    int ret = httpd_req_recv(req, buf, sizeof(buf));
    if (ret <= 0)
    {
        ESP_LOGI(TAG, "Failed to receive data");
        return ESP_FAIL;
    }

    buf[ret] = '\0'; // Null-terminate the buffer
    ESP_LOGI(TAG, "Received control action data: %s", buf);

    // Parse the incoming JSON data
    cJSON *data = cJSON_Parse(buf);
    if (data == NULL)
    {
        ESP_LOGE(TAG, "Failed to parse JSON data");
        return ESP_FAIL;
    }

    const cJSON *deviceName = cJSON_GetObjectItem(data, "deviceName");
    const cJSON *deviceAction = cJSON_GetObjectItem(data, "deviceAction");
    const cJSON *deviceAddress = cJSON_GetObjectItem(data, "deviceAddress");
    const cJSON *deviceCommand = cJSON_GetObjectItem(data, "deviceCommand");

    if (cJSON_IsString(deviceName) && cJSON_IsString(deviceAction))
    {
        ESP_LOGI(TAG, "Device Name: %s", deviceName->valuestring);
        ESP_LOGI(TAG, "Device Action: %s", deviceAction->valuestring);
        ESP_LOGI(TAG, "Device Address: %x", deviceAddress->valueint);
        ESP_LOGI(TAG, "Device Command: %x", deviceCommand->valueint);

        ir_send_command(deviceAddress->valueint, deviceCommand->valueint);

        // Here you can add your logic to control the device, e.g., send commands to GPIO, etc.

        const char *response = "{\"status\":\"success\"}";
        httpd_resp_send(req, response, strlen(response));
    }
    else
    {
        const char *response = "{\"status\":\"error\", \"message\":\"Invalid JSON\"}";
        httpd_resp_send(req, response, strlen(response));
    }

    cJSON_Delete(data); // Clean up the JSON object
    return ESP_OK;
}
esp_err_t search_device_handler(httpd_req_t *req)
{
    int result = search_for_device(); // Call device search function
    ESP_LOGI(TAG, "The result %d", result);
    // Create JSON response
    cJSON *response = cJSON_CreateObject();
    if (result == -1)
    {
        ESP_LOGI("TAG", "DATA NOT RECEIVED IN TIME");
        cJSON_AddNumberToObject(response, "result", -1);
    }
    if (result > 0)
    {
        cJSON_AddNumberToObject(response, "result", 1);
        ESP_LOGI("TAG", "DATA  RECEIVED IN TIME: %d", result);
    }

    const char *json_response = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_response, strlen(json_response));

    // Cleanup
    cJSON_Delete(response);
    free((void *)json_response);

    return ESP_OK;
}

httpd_uri_t icon_uri = {
    .uri = "/icon.png",
    .method = HTTP_GET,
    .handler = icon_handler,
    .user_ctx = NULL};

httpd_uri_t uri_get = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = get_handler,
    .user_ctx = NULL};
httpd_uri_t web_page_uri = {
    .uri = "/wifi",
    .method = HTTP_GET,
    .handler = wifi_cred_handler,
    .user_ctx = NULL};

httpd_uri_t control_device_uri = {
    .uri = "/control_device",
    .method = HTTP_GET,
    .handler = control_device_handler,
    .user_ctx = NULL};

httpd_uri_t add_device_uri = {
    .uri = "/add_device",
    .method = HTTP_GET,
    .handler = add_device_handler,
    .user_ctx = NULL};
httpd_uri_t uri_on = {
    .uri = "/on",
    .method = HTTP_GET,
    .handler = on_handler,
    .user_ctx = NULL};

httpd_uri_t uri_off = {
    .uri = "/off",
    .method = HTTP_GET,
    .handler = off_handler,
    .user_ctx = NULL};
httpd_uri_t search_device_uri = {
    .uri = "/search_device",
    .method = HTTP_POST,
    .handler = search_device_handler,
    .user_ctx = NULL};

httpd_uri_t submit_name_uri = {
    .uri = "/submit_button",
    .method = HTTP_POST,
    .handler = submit_handler,
    .user_ctx = NULL};

httpd_uri_t submit_add_device_uri = {
    .uri = "/submit_add_device",
    .method = HTTP_POST,
    .handler = submit_add_device_handler,
    .user_ctx = NULL};

httpd_uri_t get_details_uri = {
    .uri = "/get_details",
    .method = HTTP_POST,
    .handler = get_device_details_handler,
    .user_ctx = NULL};

httpd_uri_t control_action_uri = {
    .uri = "/control_action",
    .method = HTTP_POST,
    .handler = control_action_uri_handler, // Register the new control action handler
    .user_ctx = NULL};

httpd_handle_t setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &control_device_uri);
        httpd_register_uri_handler(server, &submit_add_device_uri);
        httpd_register_uri_handler(server, &web_page_uri);
        httpd_register_uri_handler(server, &icon_uri);
        httpd_register_uri_handler(server, &submit_name_uri);
        httpd_register_uri_handler(server, &add_device_uri);
        httpd_register_uri_handler(server, &search_device_uri);
        httpd_register_uri_handler(server, &get_details_uri);
        httpd_register_uri_handler(server, &control_action_uri);
    }

    return server;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED)
    {
        ESP_LOGI(TAG, "New station joined");
    }
    else if (event_id == WIFI_EVENT_AP_STADISCONNECTED)
    {
        ESP_LOGI(TAG, "A station left");
    }
}

static void setup_wifi()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *p_netif = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ipInfo;
    esp_netif_set_ip4_addr(&ipInfo.ip, 192, 168, 1, 1);
    esp_netif_set_ip4_addr(&ipInfo.gw, 192, 168, 1, 1);
    esp_netif_set_ip4_addr(&ipInfo.netmask, 255, 255, 255, 0);
    esp_netif_dhcps_stop(p_netif);
    esp_netif_set_ip_info(p_netif, &ipInfo);
    esp_netif_dhcps_start(p_netif);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = WIFI_SSID,
            .ssid_len = strlen(WIFI_SSID),
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASSWORD,
            .max_connection = WIFI_MAX_STA,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK},
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi init done.");

    esp_netif_ip_info_t if_info;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(p_netif, &if_info));
    ESP_LOGI(TAG, "ESP32 IP:" IPSTR, IP2STR(&if_info.ip));
}

void app_main(void)
{

    // Initialize NVS
    nvs_flash_init();

    // Initialize SPIFFS
    init_spiffs();

    led_state = 0;
    setup_wifi();
    setup_server();
    setup_mdns();
    init_uart();

    ESP_LOGI(TAG, "create RMT RX channel");
    rmt_rx_channel_config_t rx_channel_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = EXAMPLE_IR_RESOLUTION_HZ,
        .mem_block_symbols = 64, // amount of RMT symbols that the channel can store at a time
        .gpio_num = EXAMPLE_IR_RX_GPIO_NUM,
    };
    rmt_channel_handle_t rx_channel = NULL;
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_channel_cfg, &rx_channel));

    ESP_LOGI(TAG, "register RX done callback");
    QueueHandle_t receive_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    assert(receive_queue);
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = example_rmt_rx_done_callback,
    };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs, receive_queue));

    // the following timing requirement is based on NEC protocol
    rmt_receive_config_t receive_config = {
        .signal_range_min_ns = 1250,     // the shortest duration for NEC signal is 560us, 1250ns < 560us, valid signal won't be treated as noise
        .signal_range_max_ns = 12000000, // the longest duration for NEC signal is 9000us, 12000000ns > 9000us, the receive won't stop early
    };

    ESP_LOGI(TAG, "create RMT TX channel");

    rmt_channel_handle_t tx_channel = NULL;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_channel_cfg, &tx_channel));

    ESP_LOGI(TAG, "modulate carrier to TX channel");
    rmt_carrier_config_t carrier_cfg = {
        .duty_cycle = 0.33,
        .frequency_hz = 38000, // 38KHz
    };
    ESP_ERROR_CHECK(rmt_apply_carrier(tx_channel, &carrier_cfg));

    // this example won't send NEC frames in a loop
    rmt_transmit_config_t transmit_config = {
        .loop_count = 0, // no loop
    };

    ESP_LOGI(TAG, "install IR NEC encoder");
    ir_nec_encoder_config_t nec_encoder_cfg = {
        .resolution = EXAMPLE_IR_RESOLUTION_HZ,
    };
    ESP_ERROR_CHECK(rmt_new_ir_nec_encoder(&nec_encoder_cfg, &nec_encoder));

    ESP_LOGI(TAG, "enable RMT TX and RX channels");
    ESP_ERROR_CHECK(rmt_enable(tx_channel));
    ESP_ERROR_CHECK(rmt_enable(rx_channel));

    // save the received RMT symbols
    rmt_symbol_word_t raw_symbols[64]; // 64 symbols should be sufficient for a standard NEC frame
    rmt_rx_done_event_data_t rx_data;
    // ready to receive
    ESP_ERROR_CHECK(rmt_receive(rx_channel, raw_symbols, sizeof(raw_symbols), &receive_config));
    print_json_file_content("/spiffs/devices.json");
    while (1)
    {
        // wait for RX done signal
        if (xQueueReceive(receive_queue, &rx_data, pdMS_TO_TICKS(1000)) == pdPASS)
        {
            // parse the receive symbols and print the result
            example_parse_nec_frame(rx_data.received_symbols, rx_data.num_symbols);
            // start receive again
            ESP_ERROR_CHECK(rmt_receive(rx_channel, raw_symbols, sizeof(raw_symbols), &receive_config));
        }
    }
}
