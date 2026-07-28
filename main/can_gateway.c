/**
 * @file can_gateway.c
 * @brief Bidirectional CAN MITM gateway - dual TWAI controller, 4-task architecture.
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

static twai_handle_t   s_can0 = NULL;   /* TWAI0 - car side          */
static twai_handle_t   s_can1 = NULL;   /* TWAI1 - actuator side     */

#define TX_QUEUE_DEPTH   32

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
    gconfig.tx_queue_len  = 32;

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

    ESP_LOGI(TAG, "CAN gateway ready - both buses up, queues allocated");
    return ESP_OK;
}

/* ─── Task: CAN gateway  (car to actuator, with wiper logic applied) ────────────── */

void can_gateway_task(void *arg)
{
    ESP_LOGI(TAG, "CAN gateway task started");

    twai_message_t msg;
    esp_err_t ret;

    while (1) {
        int can0_cnt = 0;
        int can1_cnt = 0;

        /* CAN0 to CAN1  (car to actuator) */
        while (can0_cnt < 32 &&
          twai_receive_v2(s_can0, &msg, 0) == ESP_OK) {
            can0_cnt++;
            if (msg.identifier == WIPER_CAN_MSG_ID) {
                wiper_logic_process_can_frame(&msg);
            }
            ret = twai_transmit_v2(s_can1, &msg, pdMS_TO_TICKS(1));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "CAN1 TX dropped  ID=0x%03lX  err=%s",
                         (unsigned long)msg.identifier, esp_err_to_name(ret));
            }
        }

        /* CAN1 to CAN0  (actuator to car, pass-through) */
        while (can1_cnt < 32 &&
          twai_receive_v2(s_can1, &msg, 0) == ESP_OK) {
            can1_cnt++;
            ret = twai_transmit_v2(s_can0, &msg, pdMS_TO_TICKS(1));
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "CAN0 TX dropped  ID=0x%03lX  err=%s",
                         (unsigned long)msg.identifier, esp_err_to_name(ret));
            }
        }

        if (can0_cnt <= 24 && can1_cnt <= 24) {
           vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}