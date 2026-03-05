/**
 * @file can_gateway.c
 * @brief Bidirectional CAN MITM gateway – dual TWAI controller, 4-task architecture.
 *
 * Task layout
 * ───────────
 *
 *  ┌─────────────────────────────────────────────────────────────────────────┐
 *  │                            ESP32-P4                                     │
 *  │                                                                         │
 *  │  CAN0 ──► can0_rx_task ──► [wiper_logic] ──► to_can1_queue              │
 *  │                                                       │                 │
 *  │                                               can1_tx_task ──► CAN1     │
 *  │                                                                         │
 *  │  CAN0 ◄── can0_tx_task ◄── to_can0_queue                                │
 *  │                                    ▲                                    │
 *  │                             can1_rx_task ◄── CAN1                       │
 *  └─────────────────────────────────────────────────────────────────────────┘
 *
 *  can0_rx_task   – Receives from CAN0 (car side). Offers the frame to
 *                   wiper_logic_process_can_frame() before forwarding.
 *                   All frames (modified or not) go to to_can1_queue.
 *
 *  can1_tx_task   – Drains to_can1_queue → transmits on CAN1 (wiper side).
 *
 *  can1_rx_task   – Receives from CAN1 (wiper side). No modification.
 *                   All frames go to to_can0_queue.
 *
 *  can0_tx_task   – Drains to_can0_queue → transmits on CAN0 (car side).
 */

#include "can_gateway.h"
#include "config.h"
#include "wiper_logic.h"

#include "driver/twai.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "string.h"

static const char *TAG = "CAN_GW";

/* ─── Internal handles & queues ─────────────────────────────────────────── */

static twai_handle_t   s_can0 = NULL;   /* TWAI0 – car side          */
static twai_handle_t   s_can1 = NULL;   /* TWAI1 – wiper actuator    */

#define TX_QUEUE_DEPTH   32

static QueueHandle_t   s_to_can1_queue = NULL;   /* car → wiper     */
static QueueHandle_t   s_to_can0_queue = NULL;   /* wiper → car     */

/* ─── Debug helper ──────────────────────────────────────────────────────── */

#if DEBUG_CAN_GATEWAY
static void log_frame(const char *label, const twai_message_t *msg)
{
    char hex[25] = {0};
    for (int i = 0; i < msg->data_length_code && i < 8; i++) {
        snprintf(hex + i * 3, 4, "%02X ", msg->data[i]);
    }
    ESP_LOGI(TAG, "%-12s  ID=0x%03lX  DLC=%d  [%s]",
             label, (unsigned long)msg->identifier,
             msg->data_length_code, hex);
}
#else
#define log_frame(label, msg)   ((void)0)
#endif

/* ─── TWAI initialisation helper ────────────────────────────────────────── */

static esp_err_t init_twai_controller(int controller_id,
                                      gpio_num_t tx_pin,
                                      gpio_num_t rx_pin,
                                      twai_handle_t *handle_out)
{
    twai_general_config_t gconfig = TWAI_GENERAL_CONFIG_DEFAULT(tx_pin, rx_pin,
                                                                 TWAI_MODE_NORMAL);
    gconfig.controller_id = controller_id;
    gconfig.rx_queue_len  = 32;
    gconfig.tx_queue_len  = 0;   /* we manage our own TX queues */

    twai_timing_config_t tconfig = CAN_BAUD_RATE;
    twai_filter_config_t fconfig = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    esp_err_t ret = twai_driver_install_v2(&gconfig, &tconfig, &fconfig, handle_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI%d install failed: %s", controller_id, esp_err_to_name(ret));
        return ret;
    }

    ret = twai_start_v2(*handle_out);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "TWAI%d start failed: %s", controller_id, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "TWAI%d ready  TX=GPIO%d  RX=GPIO%d",
             controller_id, tx_pin, rx_pin);
    return ESP_OK;
}

/* ─── Public: init ───────────────────────────────────────────────────────── */

esp_err_t can_gateway_init(void)
{
    esp_err_t ret;

    ret = init_twai_controller(0, CAN_CAR_TX_PIN,   CAN_CAR_RX_PIN,   &s_can0);
    if (ret != ESP_OK) return ret;

    ret = init_twai_controller(1, CAN_WIPER_TX_PIN, CAN_WIPER_RX_PIN, &s_can1);
    if (ret != ESP_OK) return ret;

    s_to_can1_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(twai_message_t));
    s_to_can0_queue = xQueueCreate(TX_QUEUE_DEPTH, sizeof(twai_message_t));

    if (!s_to_can1_queue || !s_to_can0_queue) {
        ESP_LOGE(TAG, "Failed to create TX queues");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "CAN gateway ready – both buses up, queues allocated");
    return ESP_OK;
}

/* ─── Task: CAN0 RX  (car → wiper, with wiper logic applied) ────────────── */

void can_gateway_can0_rx_task(void *arg)
{
    ESP_LOGI(TAG, "CAN0 RX task started (car side)");

    twai_message_t msg;

    while (1) {
        if (twai_receive_v2(s_can0, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }

        log_frame("CAN0→CAN1", &msg);

        /*
         * Give wiper logic a chance to modify the frame.
         * Frames that don't match WIPER_CAN_MSG_ID are returned untouched.
         */
        wiper_logic_process_can_frame(&msg);

        if (xQueueSend(s_to_can1_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "to_can1_queue full – frame 0x%03lX dropped",
                     (unsigned long)msg.identifier);
        }
    }
}

/* ─── Task: CAN1 TX  (drain queue → wiper actuator) ─────────────────────── */

void can_gateway_can1_tx_task(void *arg)
{
    ESP_LOGI(TAG, "CAN1 TX task started (wiper side)");

    twai_message_t msg;

    while (1) {
        if (xQueueReceive(s_to_can1_queue, &msg, portMAX_DELAY) == pdTRUE) {
            esp_err_t ret = twai_transmit_v2(s_can1, &msg, pdMS_TO_TICKS(20));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "CAN1 TX error 0x%03lX: %s",
                         (unsigned long)msg.identifier, esp_err_to_name(ret));
            }
        }
    }
}

/* ─── Task: CAN1 RX  (wiper actuator → car, pass-through) ───────────────── */

void can_gateway_can1_rx_task(void *arg)
{
    ESP_LOGI(TAG, "CAN1 RX task started (wiper side)");

    twai_message_t msg;

    while (1) {
        if (twai_receive_v2(s_can1, &msg, portMAX_DELAY) != ESP_OK) {
            continue;
        }

        log_frame("CAN1→CAN0", &msg);

        /* Pass-through: no modification on the return path */
        if (xQueueSend(s_to_can0_queue, &msg, 0) != pdTRUE) {
            ESP_LOGW(TAG, "to_can0_queue full – frame 0x%03lX dropped",
                     (unsigned long)msg.identifier);
        }
    }
}

/* ─── Task: CAN0 TX  (drain queue → car ECU) ────────────────────────────── */

void can_gateway_can0_tx_task(void *arg)
{
    ESP_LOGI(TAG, "CAN0 TX task started (car side)");

    twai_message_t msg;

    while (1) {
        if (xQueueReceive(s_to_can0_queue, &msg, portMAX_DELAY) == pdTRUE) {
            esp_err_t ret = twai_transmit_v2(s_can0, &msg, pdMS_TO_TICKS(20));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "CAN0 TX error 0x%03lX: %s",
                         (unsigned long)msg.identifier, esp_err_to_name(ret));
            }
        }
    }
}
