/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/types.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/device.h>

#include "app_servo_motor_360.h"
#include <zephyr/bluetooth/services/trs/trs_server.h>

LOG_MODULE_REGISTER(app_servo_motor);

#if DT_NODE_HAS_STATUS(DT_ALIAS(pwm_servo), okay)
static const struct pwm_dt_spec servo_pwm = PWM_DT_SPEC_GET(DT_ALIAS(pwm_servo));
#define SERVO_PWM_AVAILABLE 1
#else
#define SERVO_PWM_AVAILABLE 0
#endif

#define SERVO_PWM_PERIOD_NS    20000000U
#define SERVO_PWM_MIN_PULSE_NS 500000U
#define SERVO_PWM_MID_PULSE_NS 1500000U
#define SERVO_PWM_MAX_PULSE_NS 2500000U

#define SERVO_360_CW_SLOW_NS   1450000U
#define SERVO_360_CCW_SLOW_NS  1550000U

static void log_vendor_cmd(const char *label, const uint8_t opcode, const uint8_t *payload, uint16_t len)
{
    char hex_str[256];
    int pos = 0;

    pos += snprintf(&hex_str[pos], sizeof(hex_str) - pos, "%s - Opcode: %02X, Len: %d, Data: ", label, opcode, len);

    for (uint16_t i = 0; i < len && pos < (int)sizeof(hex_str) - 4; i++) {
        pos += snprintf(&hex_str[pos], sizeof(hex_str) - pos, "%02X ", payload[i]);
    }

    LOG_INF("%s", hex_str);
}

static struct {
    char name[32];
    uint8_t name_len;
    uint16_t slider_value;
    uint8_t fixed_direction;
    uint8_t start_stop;
    uint32_t current_pulse_ns;
} servo_state;

static int servo_set_pwm_from_slider(uint16_t slider_value)
{
#if !SERVO_PWM_AVAILABLE
    return -ENODEV;
#else
    uint32_t pulse_ns;

    if (slider_value == 0 || servo_state.start_stop == SERVO_STOP) {
        pulse_ns = SERVO_PWM_MID_PULSE_NS;
    } else if (servo_state.fixed_direction == SERVO_DIR_CW) {
        pulse_ns = SERVO_360_CW_SLOW_NS -
                   ((uint32_t)slider_value * (SERVO_360_CW_SLOW_NS - SERVO_PWM_MIN_PULSE_NS)) / 100U;
    } else {
        pulse_ns = SERVO_360_CCW_SLOW_NS +
                   ((uint32_t)slider_value * (SERVO_PWM_MAX_PULSE_NS - SERVO_360_CCW_SLOW_NS)) / 100U;
    }

    int ret = pwm_set_dt(&servo_pwm, SERVO_PWM_PERIOD_NS, pulse_ns);
    if (ret == 0) {
        servo_state.current_pulse_ns = pulse_ns;
        LOG_INF("PWM set: speed=%u dir=%s, pulse=%u ns", slider_value,
                servo_state.fixed_direction == SERVO_DIR_CW ? "CW" : "CCW", pulse_ns);
    }
    return ret;
#endif
}

int app_servo_motor_ble_init(void)
{
    memset(&servo_state, 0, sizeof(servo_state));

    const char *default_name = "SERVO 360";
    servo_state.name_len = strlen(default_name);
    memcpy(servo_state.name, default_name, servo_state.name_len);

    servo_state.slider_value = 50;
    servo_state.fixed_direction = SERVO_DIR_CW;
    servo_state.start_stop = SERVO_STOP;

    LOG_INF("Servo motor BLE module initialized (type: 360-degree)");

#if !SERVO_PWM_AVAILABLE
    LOG_WRN("PWM servo alias not defined in devicetree, servo control disabled");
    return -ENODEV;
#else
    if (!pwm_is_ready_dt(&servo_pwm)) {
        LOG_ERR("PWM device not ready");
        return -ENODEV;
    }

    servo_state.current_pulse_ns = SERVO_PWM_MID_PULSE_NS;
    int ret = pwm_set_dt(&servo_pwm, SERVO_PWM_PERIOD_NS, SERVO_PWM_MID_PULSE_NS);
    LOG_INF("Motor set to default state: stopped (1500us pulse, no rotation)");

    if (ret != 0) {
        LOG_ERR("Failed to set PWM: %d", ret);
        return ret;
    }

    return 0;
#endif
}

static int handle_ui_update_request(struct bt_conn *conn)
{
    LOG_INF("=== Handle UI Update Request (0xA1) ===");

    int ret = app_servo_motor_ble_send_ack(conn, SERVO_ACK_UI_UPDATE);
    if (ret != 0) {
        LOG_ERR("Failed to send UI update ACK: %d", ret);
        return ret;
    }

    return app_servo_motor_ble_send_ui_update_response(conn);
}

static int handle_slider_set(struct bt_conn *conn, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < 2) {
        LOG_ERR("Invalid slider set payload length: %d", payload_len);
        return -EINVAL;
    }

    LOG_INF("=== Handle Slider Set (0xA2) ===");

    uint16_t slider_value = (payload[0] << 8) | payload[1];

    if (slider_value > 100) {
        LOG_ERR("Invalid slider value for 360-degree servo: %d", slider_value);
        return -EINVAL;
    }

    servo_state.slider_value = slider_value;

    LOG_INF("Slider set to: %d", slider_value);

    int ret = app_servo_motor_ble_send_ack(conn, SERVO_ACK_SLIDER_SET);
    if (ret != 0) {
        LOG_ERR("Failed to send slider set ACK: %d", ret);
        return ret;
    }

    ret = servo_set_pwm_from_slider(slider_value);
    if (ret != 0) {
        LOG_ERR("Failed to set PWM: %d", ret);
    }

    return app_servo_motor_ble_send_slider_response(conn, slider_value);
}

static int handle_fixed_direction_set(struct bt_conn *conn, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < 1) {
        LOG_ERR("Invalid fixed direction set payload length: %d", payload_len);
        return -EINVAL;
    }

    LOG_INF("=== Handle Fixed Direction Set (0xA4) ===");

    uint8_t direction = payload[0];

    if (direction != SERVO_DIR_CW && direction != SERVO_DIR_CCW) {
        LOG_ERR("Invalid fixed direction value: 0x%02X", direction);
        return -EINVAL;
    }

    servo_state.fixed_direction = direction;

    LOG_INF("Fixed direction set to: %s", direction == SERVO_DIR_CW ? "CW" : "CCW");

    int ret = app_servo_motor_ble_send_ack(conn, SERVO_ACK_FIXED_DIRECTION_SET);
    if (ret != 0) {
        LOG_ERR("Failed to send fixed direction set ACK: %d", ret);
        return ret;
    }

    ret = servo_set_pwm_from_slider(servo_state.slider_value);
    if (ret != 0) {
        LOG_ERR("Failed to set PWM: %d", ret);
    }

    return app_servo_motor_ble_send_fixed_direction_response(conn, direction);
}

static int handle_start_stop_set(struct bt_conn *conn, const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len < 1) {
        LOG_ERR("Invalid start/stop set payload length: %d", payload_len);
        return -EINVAL;
    }

    LOG_INF("=== Handle START/STOP Set (0xA5) ===");

    uint8_t start_stop = payload[0];

    if (start_stop != SERVO_STOP && start_stop != SERVO_START) {
        LOG_ERR("Invalid start/stop value: 0x%02X", start_stop);
        return -EINVAL;
    }

    servo_state.start_stop = start_stop;

    LOG_INF("Start/stop set to: %s", start_stop == SERVO_START ? "START" : "STOP");

    int ret = app_servo_motor_ble_send_ack(conn, SERVO_ACK_START_STOP_SET);
    if (ret != 0) {
        LOG_ERR("Failed to send start/stop set ACK: %d", ret);
        return ret;
    }

    ret = servo_set_pwm_from_slider(servo_state.slider_value);
    if (ret != 0) {
        LOG_ERR("Failed to set PWM: %d", ret);
    }

    return app_servo_motor_ble_send_start_stop_response(conn, start_stop);
}

int app_servo_motor_ble_handle_cmd(struct bt_conn *conn, uint8_t opcode,
                                   const uint8_t *payload, uint16_t payload_len)
{
    if (opcode != SERVO_MOTOR_OPCODE) {
        LOG_ERR("Invalid opcode: 0x%02X, expected 0x%02X", opcode, SERVO_MOTOR_OPCODE);
        return -EINVAL;
    }

    if (!payload || payload_len < 2) {
        LOG_ERR("Invalid payload length: %d (expected at least 2)", payload_len);
        return -EINVAL;
    }

    uint8_t cmd_length = payload[0];
    uint8_t cmd_id = payload[1];
    const uint8_t *cmd_payload = payload + 2;
    uint16_t cmd_payload_len = payload_len - 2;

    LOG_INF("=== Vendor Command Received ===");
    LOG_INF("Decoded vendor command length=%u id=0x%02X payload_len=%u", cmd_length, cmd_id, cmd_payload_len);
    log_vendor_cmd("RX", opcode, payload, payload_len);

    if (cmd_length < 1 || cmd_length != (uint8_t)(cmd_payload_len + 1)) {
        LOG_WRN("Vendor command length mismatch: length_field=%u expected=%u", cmd_length, cmd_payload_len + 1);
        if (cmd_length > (uint8_t)(payload_len - 1)) {
            return -EINVAL;
        }
    }

    switch (cmd_id) {
    case SERVO_CMD_UI_UPDATE_REQ:
        return handle_ui_update_request(conn);

    case SERVO_CMD_SLIDER_SET:
        return handle_slider_set(conn, cmd_payload, cmd_payload_len);

    case SERVO_CMD_FIXED_DIRECTION_SET:
        return handle_fixed_direction_set(conn, cmd_payload, cmd_payload_len);

    case SERVO_CMD_START_STOP_SET:
        return handle_start_stop_set(conn, cmd_payload, cmd_payload_len);

    default:
        LOG_ERR("Unknown command ID: 0x%02X", cmd_id);
        return -EINVAL;
    }
}

int app_servo_motor_ble_send_ui_update_response(struct bt_conn *conn)
{
    uint8_t payload[64];
    uint16_t payload_len = 1;

    payload[0] = 0;
    payload[payload_len++] = SERVO_RSP_UI_UPDATE;
    payload[payload_len++] = servo_state.name_len;

    memcpy(&payload[payload_len], servo_state.name, servo_state.name_len);
    payload_len += servo_state.name_len;

    payload[payload_len++] = SERVO_MOTOR_TYPE;

    payload[payload_len++] = (servo_state.slider_value >> 8) & 0xFF;
    payload[payload_len++] = servo_state.slider_value & 0xFF;

    payload[payload_len++] = servo_state.fixed_direction;
    payload[payload_len++] = servo_state.start_stop;

    payload[0] = payload_len - 1;

    LOG_INF("=== Sending UI Update Response (0xB1) ===");
    log_vendor_cmd("TX", SERVO_MOTOR_OPCODE, payload, payload_len);

    return trs_server_send_vendor_command(conn, SERVO_MOTOR_OPCODE, payload_len, payload);
}

int app_servo_motor_ble_send_slider_response(struct bt_conn *conn, uint16_t slider_value)
{
    uint8_t payload[5];

    payload[0] = 3;
    payload[1] = SERVO_RSP_SLIDER_SET;
    payload[2] = (slider_value >> 8) & 0xFF;
    payload[3] = slider_value & 0xFF;

    LOG_INF("=== Sending Slider Response (0xB2) - Value: %d ===", slider_value);
    log_vendor_cmd("TX", SERVO_MOTOR_OPCODE, payload, 4);

    return trs_server_send_vendor_command(conn, SERVO_MOTOR_OPCODE, 4, payload);
}

int app_servo_motor_ble_send_fixed_direction_response(struct bt_conn *conn, uint8_t direction)
{
    uint8_t payload[3];

    payload[0] = 2;
    payload[1] = SERVO_RSP_FIXED_DIRECTION_SET;
    payload[2] = direction;

    LOG_INF("=== Sending Fixed Direction Response (0xB4) - Direction: %s ===",
            direction == SERVO_DIR_CW ? "CW" : "CCW");
    log_vendor_cmd("TX", SERVO_MOTOR_OPCODE, payload, 3);

    return trs_server_send_vendor_command(conn, SERVO_MOTOR_OPCODE, 3, payload);
}

int app_servo_motor_ble_send_start_stop_response(struct bt_conn *conn, uint8_t start_stop)
{
    uint8_t payload[3];

    payload[0] = 2;
    payload[1] = SERVO_RSP_START_STOP_SET;
    payload[2] = start_stop;

    LOG_INF("=== Sending START/STOP Response (0xB5) - State: %s ===",
            start_stop == SERVO_START ? "START" : "STOP");
    log_vendor_cmd("TX", SERVO_MOTOR_OPCODE, payload, 3);

    return trs_server_send_vendor_command(conn, SERVO_MOTOR_OPCODE, 3, payload);
}

int app_servo_motor_ble_send_slider_notify(struct bt_conn *conn, uint16_t slider_value)
{
    uint8_t payload[4];

    payload[0] = 3;
    payload[1] = SERVO_NTF_SLIDER;
    payload[2] = (slider_value >> 8) & 0xFF;
    payload[3] = slider_value & 0xFF;

    LOG_INF("=== Sending Slider Notification (0xC2) - Value: %d ===", slider_value);
    log_vendor_cmd("TX", SERVO_MOTOR_OPCODE, payload, 4);

    return trs_server_send_vendor_command(conn, SERVO_MOTOR_OPCODE, 4, payload);
}

int app_servo_motor_ble_send_fixed_direction_notify(struct bt_conn *conn, uint8_t direction)
{
    uint8_t payload[3];

    payload[0] = 2;
    payload[1] = SERVO_NTF_FIXED_DIRECTION;
    payload[2] = direction;

    LOG_INF("=== Sending Fixed Direction Notification (0xC4) - Direction: %s ===",
            direction == SERVO_DIR_CW ? "CW" : "CCW");
    log_vendor_cmd("TX", SERVO_MOTOR_OPCODE, payload, 3);

    return trs_server_send_vendor_command(conn, SERVO_MOTOR_OPCODE, 3, payload);
}

int app_servo_motor_ble_send_start_stop_notify(struct bt_conn *conn, uint8_t start_stop)
{
    uint8_t payload[3];

    payload[0] = 2;
    payload[1] = SERVO_NTF_START_STOP;
    payload[2] = start_stop;

    LOG_INF("=== Sending START/STOP Notification (0xC5) - State: %s ===",
            start_stop == SERVO_START ? "START" : "STOP");
    log_vendor_cmd("TX", SERVO_MOTOR_OPCODE, payload, 3);

    return trs_server_send_vendor_command(conn, SERVO_MOTOR_OPCODE, 3, payload);
}

int app_servo_motor_ble_send_ack(struct bt_conn *conn, uint8_t ack_id)
{
    if (ack_id != SERVO_ACK_UI_UPDATE &&
        ack_id != SERVO_ACK_SLIDER_SET &&
        ack_id != SERVO_ACK_FIXED_DIRECTION_SET &&
        ack_id != SERVO_ACK_START_STOP_SET) {
        LOG_ERR("Invalid ACK ID: 0x%02X", ack_id);
        return -EINVAL;
    }

    uint8_t payload[2];
    payload[0] = 1;
    payload[1] = ack_id;

    LOG_INF("=== Sending ACK (0x%02X) ===", ack_id);
    log_vendor_cmd("TX", SERVO_MOTOR_OPCODE, payload, 2);

    return trs_server_send_vendor_command(conn, SERVO_MOTOR_OPCODE, 2, payload);
}
