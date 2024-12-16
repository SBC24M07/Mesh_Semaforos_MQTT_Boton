/* Mesh Internal Communication Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#ifndef __TRAFFIC_LIGHT_H__
#define _TRAFFIC_LIGHT_H__

#include "esp_err.h"

/*******************************************************
 *                Constants
 *******************************************************/
#define TRAFFIC_LIGHT_RED       (0xff)
#define TRAFFIC_LIGHT_YELLOW    (0xfe)
#define TRAFFIC_LIGHT_GREEN     (0xfd)
#define TRAFFIC_LIGHT_INIT      (0xfa)
#define TRAFFIC_LIGHT_WARNING   (0xf9)

#define  CMD_TRAFFIC_LIGHT    (0x62)

#define BUTTON_PIN GPIO_NUM_18
#define INFRA_SENSOR_PIN GPIO_NUM_5
#define MOVEMENT_PIN GPIO_NUM_22

/*******************************************************
 *                Type Definitions
 *******************************************************/

/*******************************************************
 *                Structures
 *******************************************************/
typedef struct {
    uint8_t cmd;
    uint8_t set;
    uint8_t state;
} mesh_traffic_light_ctl_t;

/*******************************************************
 *                Variables Declarations
 *******************************************************/


/*******************************************************
 *                Function Definitions
 *******************************************************/
esp_err_t traffic_light_init(void);
esp_err_t traffic_button_init(void);
esp_err_t infrared_sensor_init(void);
esp_err_t movement_sensor_init(void);
esp_err_t traffic_light_set(int color);
esp_err_t pedestrian_traffic_light_set(int color);
esp_err_t traffic_light_process(mesh_addr_t *from, uint8_t *buf, uint16_t len);
void traffic_light_state(int state);
void pedestrian_traffic_light_state(int state);


#endif /* __TRAFFIC_LIGHT_H__ */
