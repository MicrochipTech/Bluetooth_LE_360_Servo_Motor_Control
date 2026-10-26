/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <zephyr/types.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/bluetooth/services/trs/trs_server.h>
#include "app_servo_motor_360.h"

LOG_MODULE_REGISTER(main);

#define UART_TX_BUF_SIZE 244
#define RING_BUF_SIZE    512
#define LED_BLINK_INTERVAL_MS 500
#define USER_LED_NODE DT_ALIAS(led3)

/* MBD legacy advertisement name and service data */
#define ADV_NAME "Zephyr_Servo-360"
#define ADV_NAME_LEN (sizeof(ADV_NAME) - 1)

#define UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)

/* incoming events and data from the bluetooth */
static K_FIFO_DEFINE(rx_queue);

#define APP_BUF_POOL_SIZE CONFIG_TRS_SERVER_BUF_POOL_SIZE
#define APP_BUF_DATA_SIZE CONFIG_TRS_SERVER_BUF_DATA_SIZE

NET_BUF_POOL_DEFINE(app_buff_pool, APP_BUF_POOL_SIZE, APP_BUF_DATA_SIZE, 0, NULL);
RING_BUF_DECLARE(rx_ring, RING_BUF_SIZE);

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
static const struct gpio_dt_spec user_led = GPIO_DT_SPEC_GET(USER_LED_NODE, gpios);

static struct bt_conn *ble_conn;
static struct k_spinlock conn_lock;
static struct k_work uart_rx_work;
static struct k_work_delayable uart_retry_work;
static struct k_work_delayable ble_retry_work;
static uint8_t pending_tx[UART_TX_BUF_SIZE];
static size_t pending_tx_len;
static struct bt_conn *tx_conn;
static struct k_work ble_rx_work;
static struct k_work_delayable adv_retry_work;
static struct k_work_delayable led_blink_work;
static bool advertising_started;
static bool led_state;  /* true = ON, false = OFF */

/*
 * ── UART accumulation buffer ──
 * Collects bytes from ISR until a delimiter (CR/LF) is seen,
 * then hands off the complete line via a work item.
 */
#define ACCUM_BUF_SIZE 128
static uint8_t accum_buf[ACCUM_BUF_SIZE];
static size_t  accum_pos;
static bool discard_uart_line;
static struct k_work process_line_work;

struct uart_line {
    size_t len;
    uint8_t data[ACCUM_BUF_SIZE];
};
K_MSGQ_DEFINE(line_queue, sizeof(struct uart_line), 4, sizeof(size_t));
K_MSGQ_DEFINE(ble_rx_conn_queue, sizeof(struct bt_conn *),
              CONFIG_TRS_SERVER_CREDIT, sizeof(void *));

static struct bt_conn *connection_ref_get(void)
{
    k_spinlock_key_t key = k_spin_lock(&conn_lock);
    struct bt_conn *conn = ble_conn ? bt_conn_ref(ble_conn) : NULL;

    k_spin_unlock(&conn_lock, key);
    return conn;
}

static bool connection_is_active(struct bt_conn *conn)
{
    k_spinlock_key_t key = k_spin_lock(&conn_lock);
    bool active = conn != NULL && ble_conn == conn;

    k_spin_unlock(&conn_lock, key);
    return active;
}

static uint8_t svc_data[] = {0xDA, 0xFE, 0xFF, 0xE1};

static struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA(BT_DATA_SVC_DATA16, svc_data, sizeof(svc_data)),
    BT_DATA(BT_DATA_NAME_COMPLETE, ADV_NAME, ADV_NAME_LEN),
};

/* ──────────────────────────────────────────────
 * Advertising
 * ────────────────────────────────────────────── */
static int start_advertising(void)
{
    int err;

    bt_le_adv_stop();
    err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_1, ad, ARRAY_SIZE(ad), NULL, 0);
    if (err) {
        LOG_ERR("Advertising failed to start (err %d)", err);
        return err;
    }
    LOG_INF("Configuration mode: waiting connections...");
    
    /* Start LED blinking for advertising state */
    led_state = false;
    gpio_pin_set_dt(&user_led, 1);  /* 1 for GPIO_ACTIVE_LOW = LED OFF */
    k_work_schedule(&led_blink_work, K_MSEC(LED_BLINK_INTERVAL_MS));
    
    return 0;
}

static void adv_retry_work_handler(struct k_work *work)
{
    int err = start_advertising();

    if (err == -ENOMEM) {
        k_work_schedule(&adv_retry_work, K_MSEC(200));
    }
}

/* ──────────────────────────────────────────────
 * LED Blink handler (for advertising state)
 * ────────────────────────────────────────────── */
static void led_blink_work_handler(struct k_work *work)
{
    struct bt_conn *conn = connection_ref_get();
    /* Only blink if still in advertising mode (not connected) */
    if (!conn && advertising_started) {
        led_state = !led_state;
        gpio_pin_set_dt(&user_led, led_state ? 0 : 1);  /* Invert for GPIO_ACTIVE_LOW */
        k_work_schedule(&led_blink_work, K_MSEC(LED_BLINK_INTERVAL_MS));
    }
    if (conn) {
        bt_conn_unref(conn);
    }
}

/* ──────────────────────────────────────────────
 * Connection callbacks
 * ────────────────────────────────────────────── */
static void connected(struct bt_conn *connected, uint8_t err)
{
    if (err) {
        LOG_ERR("Connection failed (err %u)", err);
    } else {
        LOG_INF("Connected");
        k_spinlock_key_t key = k_spin_lock(&conn_lock);
        if (!ble_conn) {
            ble_conn = bt_conn_ref(connected);
        }
        k_spin_unlock(&conn_lock, key);
        /* Cancel blinking and turn LED ON */
        k_work_cancel_delayable(&led_blink_work);
        led_state = true;
        gpio_pin_set_dt(&user_led, 1);  /* 1 = LED ON */
        LOG_INF("LED ON (connected)");
    }
}

static void disconnected(struct bt_conn *disconn, uint8_t reason)
{
    struct bt_conn *to_unref = NULL;
    k_spinlock_key_t key = k_spin_lock(&conn_lock);

    if (ble_conn == disconn) {
        to_unref = ble_conn;
        ble_conn = NULL;
    }
    k_spin_unlock(&conn_lock, key);
    if (to_unref) {
        bt_conn_unref(to_unref);
    }
    k_work_submit(&uart_rx_work);
    LOG_INF("Disconnected, reason %u %s", reason, bt_hci_err_to_str(reason));
    
    /* Start blinking again for re-advertising */
    led_state = false;
    gpio_pin_set_dt(&user_led, 1);  /* 1 for GPIO_ACTIVE_LOW = LED OFF */
    k_work_schedule(&led_blink_work, K_MSEC(LED_BLINK_INTERVAL_MS));
    
    k_work_schedule(&adv_retry_work, K_MSEC(200));
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
};

/* ──────────────────────────────────────────────
 * Hex conversion helper
 * ────────────────────────────────────────────── */
static int hex_char_val(uint8_t c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}


/**
 * Check if the entire buffer (ignoring whitespace/CR/LF) is valid hex pairs.
 * Returns the number of decoded bytes, or 0 if not valid hex.
 */
static int try_decode_hex(const uint8_t *buf, size_t len, uint8_t *out, size_t out_max)
{
    int count = 0;
    int nibble = -1;

    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];

        /* skip whitespace and delimiters */
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }

        int val = hex_char_val(c);

        if (val < 0) {
            /* non-hex character found → not a hex command */
            return 0;
        }

        if (nibble < 0) {
            nibble = val;
        } else {
            if ((size_t)count >= out_max) {
                return 0; /* overflow */
            }
            out[count++] = (uint8_t)((nibble << 4) | val);
            nibble = -1;
        }
    }

    /* If there's a leftover nibble, the input wasn't aligned hex pairs */
    if (nibble >= 0) {
        return 0;
    }

    return count;
}

/* ──────────────────────────────────────────────
 * Vendor command work handler (system workqueue)
 * ────────────────────────────────────────────── */
static void send_uart_vendor_command(uint8_t *data, size_t len)
{
    struct bt_conn *conn = connection_ref_get();
    if (!conn) {
        LOG_WRN("No BLE connection, discarding vendor command");
        return;
    }
    int ret = trs_server_send_vendor_command(conn, data[0], len - 1,
                                             len > 1 ? &data[1] : NULL);
    if (ret != 0) {
        LOG_ERR("Vendor send failed: %d", ret);
    }
    bt_conn_unref(conn);
}

static void tx_connection_set(struct bt_conn *conn)
{
    /* UART transmit state is owned by the system workqueue. */
    if (tx_conn == conn) {
        return;
    }
    if (pending_tx_len > 0 || !ring_buf_is_empty(&rx_ring)) {
        LOG_WRN("Discarding UART data for previous BLE connection");
    }
    pending_tx_len = 0;
    ring_buf_reset(&rx_ring);
    k_work_cancel_delayable(&uart_retry_work);
    if (tx_conn) {
        bt_conn_unref(tx_conn);
    }
    tx_conn = conn ? bt_conn_ref(conn) : NULL;
}

/* ──────────────────────────────────────────────
 * Process a complete UART line (work handler)
 *
 * Decision logic:
 *   a. If the line is valid hex AND first byte == 0x8E → vendor command
 *   b. Otherwise → transparent data (send raw ASCII over data pipe)
 * ────────────────────────────────────────────── */
static void process_uart_line(const uint8_t *local_buf, size_t local_len)
{
    if (local_len == 0) {
        return;
    }

    LOG_HEXDUMP_DBG(local_buf, local_len, "UART line");

    /* ── Try to decode as hex vendor command ── */
    uint8_t hex_bytes[64];
    int hex_len = try_decode_hex(local_buf, local_len, hex_bytes, sizeof(hex_bytes));

    if (hex_len > 0 && hex_bytes[0] == 0x8E) {
        /* This is a vendor command → send via control pipe */
        send_uart_vendor_command(hex_bytes, hex_len);
        return;
    }

    /* ── Regular transparent data → ring buffer → BLE data pipe ── */
    struct bt_conn *conn = connection_ref_get();
    if (!conn) {
        LOG_WRN("No BLE connection, discarding data");
        return;
    }
    tx_connection_set(conn);
    bt_conn_unref(conn);

    size_t stored = ring_buf_put(&rx_ring, local_buf, local_len);
    if (stored != local_len) {
        LOG_ERR("Ring buffer overflow, %zu bytes lost", local_len - stored);
    }
    if (stored > 0) {
        k_work_submit(&uart_rx_work);
    }
}

static void process_line_work_handler(struct k_work *work)
{
    struct uart_line line;

    while (k_msgq_get(&line_queue, &line, K_NO_WAIT) == 0) {
        process_uart_line(line.data, line.len);
    }
}

static void uart_retry_work_handler(struct k_work *work)
{
    k_work_submit(&uart_rx_work);
}

static void ble_retry_work_handler(struct k_work *work)
{
    k_work_submit(&ble_rx_work);
}

/* ──────────────────────────────────────────────
 * UART -> BLE (data pipe)
 * ────────────────────────────────────────────── */
static void uart_rx_work_handler(struct k_work *work)
{
    uint16_t tx_mtu = 0U;
    struct bt_conn *conn = connection_ref_get();

    tx_connection_set(conn);
    if (!conn) {
        pending_tx_len = 0;
        ring_buf_reset(&rx_ring);
        k_work_cancel_delayable(&uart_retry_work);
        return;
    }

    trs_server_get_tx_mtu(conn, &tx_mtu);
    if (tx_mtu <= 4U) {
        LOG_WRN("TRS connection not ready for UART transmission");
        goto out;
    }
    size_t payload_mtu = MIN((size_t)(tx_mtu - 4U), sizeof(pending_tx));

    while (connection_is_active(conn)) {
        if (pending_tx_len == 0) {
            pending_tx_len = ring_buf_get(&rx_ring, pending_tx, payload_mtu);
        }
        if (pending_tx_len == 0) {
            break;
        }
        size_t send_len = MIN(pending_tx_len, payload_mtu);
        int err = trs_server_send_data(conn, send_len, pending_tx);
        if (err < 0) {
            if (err == -ENOBUFS || err == -ENOTSUP || err == -EAGAIN) {
                k_work_reschedule(&uart_retry_work, K_MSEC(10));
            } else {
                LOG_ERR("Failed to send through BLE: %d", err);
                pending_tx_len = 0;
            }
            break;
        }
        pending_tx_len -= send_len;
        memmove(pending_tx, &pending_tx[send_len], pending_tx_len);
    }
out:
    bt_conn_unref(conn);
}

static void ble_rx_work_handler(struct k_work *work)
{
    struct bt_conn *conn;

    while (k_msgq_get(&ble_rx_conn_queue, &conn, K_NO_WAIT) == 0) {
        while (connection_is_active(conn)) {
            uint16_t data_len = 0;
            trs_server_get_data_len(conn, &data_len);
            if (data_len == 0) {
                break;
            }
            struct net_buf *rec_buf = net_buf_alloc_len(&app_buff_pool,
                                                       (size_t)data_len,
                                                       K_NO_WAIT);
            if (rec_buf == NULL) {
                if (k_msgq_put(&ble_rx_conn_queue, &conn, K_NO_WAIT) == 0) {
                    k_work_reschedule(&ble_retry_work, K_MSEC(10));
                } else {
                    bt_conn_unref(conn);
                    LOG_ERR("BLE receive event queue full during retry");
                }
                return;
            }

            if (trs_server_get_data(conn, net_buf_add(rec_buf, data_len)) > 0) {
                unsigned int key = irq_lock();
                bool was_empty = k_fifo_is_empty(&rx_queue);

                k_fifo_put(&rx_queue, rec_buf);
                if (was_empty) {
                    uart_irq_tx_enable(uart_dev);
                }
                irq_unlock(key);
            } else {
                net_buf_unref(rec_buf);
            }
        }
        bt_conn_unref(conn);
    }
}

/* ──────────────────────────────────────────────
 * TRS server callback
 * ────────────────────────────────────────────── */
static void trs_server_cb_fn(struct trs_server_event *event)
{
    switch (event->event_id) {
    case TRS_SERVER_EVT_RECEIVE_DATA: {
        struct bt_conn *conn = bt_conn_ref(event->event_field.conn);
        if (k_msgq_put(&ble_rx_conn_queue, &conn, K_NO_WAIT) != 0) {
            bt_conn_unref(conn);
            LOG_ERR("BLE receive event queue full");
            break;
        }
        k_work_submit(&ble_rx_work);
        break;
    }

    case TRS_SERVER_EVT_TX_STATUS:
        if (event->event_field.tx_status.status == TRS_SERVER_STATUS_TX_OPENED) {
            k_work_submit(&uart_rx_work);
        }
        break;

    case TRS_SERVER_EVT_CTRL_STATUS:
        LOG_INF("Control Point status: %u", event->event_field.ctrl_status.status);
        break;

    case TRS_SERVER_EVT_CBFC_ENABLED:
        LOG_INF("CBFC Rx enabled");
        break;

    case TRS_SERVER_EVT_CBFC_CREDIT:
        LOG_INF("CBFC Tx credit updated");
        k_work_submit(&uart_rx_work);
        break;

    case TRS_SERVER_EVT_VENDOR_CMD: {
        struct bt_conn *conn = event->event_field.conn;
        uint8_t *vdata = event->event_field.vendor_cmd.payload;
        uint16_t vlen  = event->event_field.vendor_cmd.length;

        if (vlen > 0 && vdata) {
            if (vdata[0] == 0x8E && conn) {
                LOG_INF("Vendor cmd received size=%u", vlen);
                LOG_HEXDUMP_INF(vdata, vlen, "RX Vendor:");

                int ret = app_servo_motor_ble_handle_cmd(conn, vdata[0],
                                                       vdata + 1, vlen - 1);
                if (ret != 0) {
                    LOG_ERR("Failed to handle servo motor command: %d", ret);
                }
            } else {
                LOG_INF("Data received size=%u", vlen);
                LOG_HEXDUMP_INF(vdata, vlen, "RX Data:");

                if (conn) {
                    uint8_t ack_data[UART_TX_BUF_SIZE];
                    const char ack_prefix[] = "data received :";
                    size_t prefix_len = sizeof(ack_prefix) - 1;
                    size_t copy_len = MIN((size_t)vlen, sizeof(ack_data) - prefix_len);

                    memcpy(ack_data, ack_prefix, prefix_len);
                    memcpy(&ack_data[prefix_len], vdata, copy_len);

                    int ack_ret = trs_server_send_data(conn,
                                                       prefix_len + copy_len,
                                                       ack_data);
                    if (ack_ret < 0) {
                        LOG_ERR("Failed to send data acknowledgement: %d", ack_ret);
                    }
                }
            }

            /* Echo to UART/Docklight */
            for (uint16_t i = 0; i < vlen; i++) {
                uart_poll_out(uart_dev, vdata[i]);
            }
        }
        break;
    }

    case TRS_SERVER_EVT_ERR_UNSPECIFIED:
        LOG_ERR("TRS Error: Unspecified");
        break;

    case TRS_SERVER_EVT_ERR_NO_MEM:
        LOG_ERR("TRS Error: No memory");
        break;

    default:
        break;
    }
}

static struct trs_server_cb trs_server_event_cb = {
    .trs_server_event = trs_server_cb_fn,
};

/* ──────────────────────────────────────────────
 * Auth
 * ────────────────────────────────────────────── */
static void auth_cancel(struct bt_conn *conn)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    printk("Pairing cancelled: %s\n", addr);
}

static struct bt_conn_auth_cb auth_cb_display = {
    .cancel = auth_cancel,
};

static void bt_ready(int err)
{
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
        return;
    }
    LOG_INF("Bluetooth initialized");
    start_advertising();
}

/* ──────────────────────────────────────────────
 * UART ISR – accumulate bytes, dispatch complete
 * lines via work item
 *
 * Delimiter: CR (\r) or LF (\n)
 *
 * For Docklight: make sure your sequences end with
 * CR or LF (or CR+LF). Configure Docklight send
 * sequences accordingly.
 * ────────────────────────────────────────────── */
static void rx_uart_isr(void)
{
    uint8_t tmp[32];
    int n = uart_fifo_read(uart_dev, tmp, sizeof(tmp));

    for (int i = 0; i < n; i++) {
        uint8_t c = tmp[i];

        if (c == '\r' || c == '\n') {
            if (discard_uart_line) {
                discard_uart_line = false;
                continue;
            }
            if (accum_pos > 0) {
                struct uart_line line = {.len = accum_pos};
                memcpy(line.data, accum_buf, accum_pos);
                if (k_msgq_put(&line_queue, &line, K_NO_WAIT) != 0) {
                    LOG_ERR("UART line queue full, dropping line");
                }
                accum_pos = 0;
                k_work_submit(&process_line_work);
            }
            /* else: ignore empty lines / consecutive delimiters */
        } else {
            if (discard_uart_line) {
                continue;
            }
            if (accum_pos < ACCUM_BUF_SIZE) {
                accum_buf[accum_pos++] = c;
            } else {
                /* Overflow – discard this line */
                LOG_ERR("Accum buffer overflow, discarding");
                accum_pos = 0;
                discard_uart_line = true;
            }
        }
    }
}

static void tx_uart_data(void)
{
    static struct net_buf *rec_buf;
    int len;

    if (!rec_buf) {
        rec_buf = k_fifo_get(&rx_queue, K_NO_WAIT);
        if (!rec_buf) {
            uart_irq_tx_disable(uart_dev);
            return;
        }
    }

    len = uart_fifo_fill(uart_dev, rec_buf->data, rec_buf->len);
    net_buf_pull(rec_buf, len);

    if (!rec_buf->len) {
        net_buf_unref(rec_buf);
        rec_buf = NULL;
        k_work_submit(&ble_rx_work);
    }
}

static void uart_isr(const struct device *unused, void *user_data)
{
    ARG_UNUSED(unused);
    ARG_UNUSED(user_data);

    while (uart_irq_update(uart_dev) && uart_irq_is_pending(uart_dev)) {
        if (uart_irq_tx_ready(uart_dev)) {
            tx_uart_data();
        }
        if (uart_irq_rx_ready(uart_dev)) {
            rx_uart_isr();
        }
    }
}

static int uart_init(void)
{
    if (!device_is_ready(uart_dev)) {
        return -ENODEV;
    }

    uart_irq_rx_disable(uart_dev);
    uart_irq_tx_disable(uart_dev);
    uart_irq_callback_set(uart_dev, uart_isr);
    uart_irq_rx_enable(uart_dev);

    return 0;
}

/* ──────────────────────────────────────────────
 * Main
 * ────────────────────────────────────────────── */
int main(void)
{
    int err;

    k_work_init(&ble_rx_work, ble_rx_work_handler);
    k_work_init(&uart_rx_work, uart_rx_work_handler);
    k_work_init_delayable(&uart_retry_work, uart_retry_work_handler);
    k_work_init_delayable(&ble_retry_work, ble_retry_work_handler);
    k_work_init(&process_line_work, process_line_work_handler);
    k_work_init_delayable(&adv_retry_work, adv_retry_work_handler);
    k_work_init_delayable(&led_blink_work, led_blink_work_handler);
    advertising_started = true;

    LOG_INF("Initializing Bluetooth...");

    err = uart_init();
    if (err) {
        return 0;
    }

    /* Initialize LED GPIO */
    if (!gpio_is_ready_dt(&user_led)) {
        LOG_ERR("LED GPIO device not ready");
        return 0;
    }
    err = gpio_pin_configure_dt(&user_led, GPIO_OUTPUT);
    if (err < 0) {
        LOG_ERR("Failed to configure LED GPIO: %d", err);
        return 0;
    }
    gpio_pin_set_dt(&user_led, 1);  /* Start with LED OFF */
    led_state = false;
    LOG_INF("LED initialized");

    bt_conn_auth_cb_register(&auth_cb_display);

    err = trs_server_cb_register(&trs_server_event_cb);
    if (err) {
        return 0;
    }

    /* Initialize servo motor BLE module */
    err = app_servo_motor_ble_init();
    if (err) {
        LOG_ERR("Failed to initialize servo motor BLE module: %d", err);
        return 0;
    }

    err = bt_enable(bt_ready);
    if (err) {
        LOG_ERR("Bluetooth init failed (err %d)", err);
    }

    return 0;
}