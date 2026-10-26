/*
 * Copyright (c) 2025 Microchip Technology Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_SERVO_MOTOR_360_H
#define APP_SERVO_MOTOR_360_H

#include <zephyr/types.h>
#include <zephyr/bluetooth/conn.h>

/* Protocol Constants */
#define SERVO_MOTOR_OPCODE              0x8E
#define SERVO_MOTOR_SERVICE_DATA        0xFFE1

/* Fixed servo type for this project */
#define SERVO_TYPE_360                  0x01
#define SERVO_MOTOR_TYPE                SERVO_TYPE_360

/* Control Request IDs (App -> Device) */
#define SERVO_CMD_UI_UPDATE_REQ         0xA1
#define SERVO_CMD_SLIDER_SET            0xA2
#define SERVO_CMD_FIXED_DIRECTION_SET   0xA4
#define SERVO_CMD_START_STOP_SET        0xA5

/* Data Response IDs (Device -> App) */
#define SERVO_RSP_UI_UPDATE             0xB1
#define SERVO_RSP_SLIDER_SET            0xB2
#define SERVO_RSP_FIXED_DIRECTION_SET   0xB4
#define SERVO_RSP_START_STOP_SET        0xB5

/* Notification IDs (Device -> App) */
#define SERVO_NTF_SLIDER                0xC2
#define SERVO_NTF_FIXED_DIRECTION       0xC4
#define SERVO_NTF_START_STOP            0xC5

/* Acknowledgment Response IDs */
#define SERVO_ACK_UI_UPDATE             0xD1
#define SERVO_ACK_SLIDER_SET            0xD2
#define SERVO_ACK_FIXED_DIRECTION_SET   0xD4
#define SERVO_ACK_START_STOP_SET        0xD5

/* Direction Values (360 servo) */
#define SERVO_DIR_CW                    0x00
#define SERVO_DIR_CCW                   0x01

/* Start/Stop Values */
#define SERVO_STOP                      0x00
#define SERVO_START                     0x01

int app_servo_motor_ble_init(void);

int app_servo_motor_ble_handle_cmd(struct bt_conn *conn, uint8_t opcode,
                                   const uint8_t *payload, uint16_t payload_len);

int app_servo_motor_ble_send_ui_update_response(struct bt_conn *conn);

int app_servo_motor_ble_send_slider_response(struct bt_conn *conn, uint16_t slider_value);

int app_servo_motor_ble_send_fixed_direction_response(struct bt_conn *conn, uint8_t direction);

int app_servo_motor_ble_send_start_stop_response(struct bt_conn *conn, uint8_t start_stop);

int app_servo_motor_ble_send_slider_notify(struct bt_conn *conn, uint16_t slider_value);

int app_servo_motor_ble_send_fixed_direction_notify(struct bt_conn *conn, uint8_t direction);

int app_servo_motor_ble_send_start_stop_notify(struct bt_conn *conn, uint8_t start_stop);

int app_servo_motor_ble_send_ack(struct bt_conn *conn, uint8_t ack_id);

#endif /* APP_SERVO_MOTOR_360_H */
