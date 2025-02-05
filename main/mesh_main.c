/* Mesh Internal Communication Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include <inttypes.h>
#include <time.h>
#include <sys/time.h>

#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mesh.h"
#include "nvs_flash.h"

#include "driver/gpio.h"
#include "freertos/semphr.h"
#include "cJSON.h"

#include "mesh_netif.h"
#include "traffic_light.h"

#include "esp_sleep.h"

/*******************************************************
 *                Macros
 *******************************************************/

// commands for internal mesh communication:
// <CMD> <PAYLOAD>, where CMD is one character, payload is variable dep. on command
#define CMD_BUTTON_PRESSED 0x55
// CMD_BUTTON_PRESSED: payload is always 6 bytes identifying address of node sending keypress event
#define CMD_ROUTE_TABLE 0x56
// CMD_BUTTON_PRESSED: payload is a multiple of 6 listing addresses in a routing table
#define CMD_MOVEMENT_DETECTED 0x57

/*******************************************************
 *                Constants
 *******************************************************/
static const char *MESH_TAG = "mesh_main";
static const uint8_t MESH_ID[6] = { 0x77, 0x77, 0x77, 0x77, 0x77, 0x76};


/*******************************************************
 *                Variable Definitions
 *******************************************************/
static mesh_addr_t mesh_parent_addr;
static int mesh_layer = -1;
static esp_ip4_addr_t s_current_ip;
static mesh_addr_t s_route_table[CONFIG_MESH_ROUTE_TABLE_SIZE];
static int s_route_table_size = 0;
static SemaphoreHandle_t s_route_table_lock = NULL;
static SemaphoreHandle_t s_traffic_button_lock = NULL;
static bool button_pressed = false;


/*******************************************************
 *                Function Declarations
 *******************************************************/
// interaction with public mqtt broker
void mqtt_app_start(void);
void mqtt_app_publish(char* topic, cJSON *json);

//Ota control
void ota_update(void);

//Time control
void obtain_time(void);
void print_current_time();

/*******************************************************
 *                Function Definitions
 *******************************************************/

void static recv_cb(mesh_addr_t *from, mesh_data_t *data)
{
	switch(data->data[0]){
		case CMD_ROUTE_TABLE:
			int size =  data->size - 1;
        	if (s_route_table_lock == NULL || size%6 != 0) {
            	ESP_LOGE(MESH_TAG, "Error in receiving raw mesh data: Unexpected size");
            	return;
        	}
        	xSemaphoreTake(s_route_table_lock, portMAX_DELAY);
        	s_route_table_size = size / 6;
        	for (int i=0; i < s_route_table_size; ++i) {
            	ESP_LOGI(MESH_TAG, "Received Routing table [%d] "
                    	MACSTR, i, MAC2STR(data->data + 6*i + 1));
        	}
        	memcpy(&s_route_table, data->data + 1, size);
        	xSemaphoreGive(s_route_table_lock);
			break;
	}
}

static void check_button(void* args)
{
    bool level_bt;
    bool level_inf;
    bool run_check_button = true;
    traffic_button_init();
    infrared_sensor_init();
    
    while (run_check_button) {
        level_bt = gpio_get_level(BUTTON_PIN);
        //ESP_LOGI(MESH_TAG, "Boton: %i", level_bt);
        level_inf = gpio_get_level(INFRA_SENSOR_PIN);
        //ESP_LOGI(MESH_TAG, "Infrarrojos %i", level_inf);
        if (!button_pressed && (level_bt || !level_inf)) {
			xSemaphoreTake(s_traffic_button_lock, portMAX_DELAY);
			button_pressed = true;
			xSemaphoreGive(s_traffic_button_lock);
            if (s_route_table_size && !esp_mesh_is_root()) {
                ESP_LOGW(MESH_TAG, "Button pressed!");
                mesh_data_t data;
                uint8_t *my_mac = mesh_netif_get_station_mac();
                uint8_t data_to_send[6+1+1] = { CMD_BUTTON_PRESSED, };
                esp_err_t err;
                memcpy(data_to_send + 1, my_mac, 6);
                data_to_send[7] = 1;
                data.size = sizeof(data_to_send);
                data.proto = MESH_PROTO_BIN;
                data.tos = MESH_TOS_P2P;
                data.data = data_to_send;
                xSemaphoreTake(s_route_table_lock, portMAX_DELAY);
				
				//enviar mensaje al maestro
                err = esp_mesh_send(&s_route_table[0], &data, MESH_DATA_P2P, NULL, 0);
                ESP_LOGI(MESH_TAG, "Sending to [%d] "
                        MACSTR ": sent with err code: %d", 0, MAC2STR(s_route_table[0].addr), err);

                xSemaphoreGive(s_route_table_lock);
                
                //Publicar en thingsboard
                cJSON *root = cJSON_CreateObject();
        		if (root == NULL) {
            		ESP_LOGE(MESH_TAG, "Failed to create JSON object");
            		vTaskDelay(1000 / portTICK_PERIOD_MS);
            		continue;
        		}

        		cJSON_AddNumberToObject(root, "button", level_bt);
        		cJSON_AddNumberToObject(root, "infrared", level_inf);
        		
				mqtt_app_publish("v1/devices/me/telemetry", root);

        		cJSON_Delete(root);
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

static void check_movement_sensor(void* args)
{
	bool level;
    movement_sensor_init();
    
    while (true) {
        level = gpio_get_level(MOVEMENT_PIN);
        //ESP_LOGI(MESH_TAG, "Movimiento: %i", level);
        if (level) {
            if (s_route_table_size && !esp_mesh_is_root()) {
                ESP_LOGW(MESH_TAG, "Movement detected!");
                mesh_data_t data;
                uint8_t *my_mac = mesh_netif_get_station_mac();
                uint8_t data_to_send[6+1+1] = { CMD_MOVEMENT_DETECTED, };
                esp_err_t err;
                //char print[6*3+1]; // MAC addr size + terminator
                memcpy(data_to_send + 1, my_mac, 6);
                data_to_send[7] = 1;
                data.size = sizeof(data_to_send);
                data.proto = MESH_PROTO_BIN;
                data.tos = MESH_TOS_P2P;
                data.data = data_to_send;
                //snprintf(print, sizeof(print),MACSTR, MAC2STR(my_mac));
                //mqtt_app_publish("/topic/ip_mesh/key_pressed", print);
                xSemaphoreTake(s_route_table_lock, portMAX_DELAY);
				
				//enviar mensaje al maestro
                err = esp_mesh_send(&s_route_table[0], &data, MESH_DATA_P2P, NULL, 0);
                ESP_LOGI(MESH_TAG, "Sending to [%d] "
                        MACSTR ": sent with err code: %d", 0, MAC2STR(s_route_table[0].addr), err);

                xSemaphoreGive(s_route_table_lock);
                
                //Publicar en thingsboard
                cJSON *root = cJSON_CreateObject();
        		if (root == NULL) {
            		ESP_LOGE(MESH_TAG, "Failed to create JSON object");
            		vTaskDelay(1000 / portTICK_PERIOD_MS);
            		continue;
        		}
        		
        		cJSON_AddNumberToObject(root, "movement", level);
		
				mqtt_app_publish("v1/devices/me/telemetry", root);

        		cJSON_Delete(root);
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

static void traffic_light_control (void* args)
{
    uint8_t color_pos = 0; //0 = verde, 1 = cambio, 2 = rojo
    bool ped_color_pos = 1; //0 = verde, 1 = rojo
    bool can_send = true;
    
    uint8_t minimum_green_timer = 10;
    uint8_t yellow_timer = 3;
    uint8_t red_timer = 10;
    uint8_t change_red_timer = 5;
    uint8_t timer = 0;
    
    
    //Inicializar el semaforo
    traffic_light_set(TRAFFIC_LIGHT_RED);
	pedestrian_traffic_light_set(TRAFFIC_LIGHT_RED);
    while (true) {
		if(button_pressed){
			switch(color_pos){
				case 0: //En verde
					if (timer <= 0){
						color_pos = 1;
						ped_color_pos = 1;
						timer = yellow_timer;
						traffic_light_set(TRAFFIC_LIGHT_YELLOW);
						can_send = true;
					} else {
						timer--;
					}
					break;
				case 1://En amarillo
					if (timer <= 0){
						color_pos = 2;
						ped_color_pos = 0;
						timer = red_timer + change_red_timer;
						traffic_light_set(TRAFFIC_LIGHT_RED);
						pedestrian_traffic_light_set(TRAFFIC_LIGHT_GREEN);
						can_send = true;
					} else {
						timer--;
					}
					break;
				case 2://En rojo
					if (timer <= 0){
						color_pos = 0;
						ped_color_pos = 1;
						timer = minimum_green_timer;
						traffic_light_set(TRAFFIC_LIGHT_GREEN);
						pedestrian_traffic_light_set(TRAFFIC_LIGHT_RED);
						xSemaphoreTake(s_traffic_button_lock, portMAX_DELAY);
						button_pressed = false;
						xSemaphoreGive(s_traffic_button_lock);
						can_send = true;
					} else {
						if (timer <= change_red_timer){
							if(ped_color_pos){
								pedestrian_traffic_light_set(0);
							} else {
								pedestrian_traffic_light_set(TRAFFIC_LIGHT_GREEN);
							}
							ped_color_pos = !ped_color_pos;
						}
						timer--;
					}
					break;
				default:
					break;
			}
			
		} else {
			traffic_light_set(TRAFFIC_LIGHT_GREEN);
			pedestrian_traffic_light_set(TRAFFIC_LIGHT_RED);
			if(timer > 0){
				timer--;
			}
		}
		
		//Publicar en thingsboard
		if (can_send){
			cJSON *root = cJSON_CreateObject();
			if (root == NULL) {
    			ESP_LOGE(MESH_TAG, "Failed to create JSON object");
    			vTaskDelay(1000 / portTICK_PERIOD_MS);
    			continue;
			}

			cJSON_AddNumberToObject(root, "semaforo_coches", color_pos);
			cJSON_AddNumberToObject(root, "semaforo_peaton", ped_color_pos);
        		
			mqtt_app_publish("v1/devices/me/telemetry", root);

			cJSON_Delete(root);
			can_send = false;
		}

		vTaskDelay(1 * 1000 / portTICK_PERIOD_MS);
    }
    vTaskDelete(NULL);
}

void ota_task(void *pvParameters){
	
	while(1){
		time_t now;
    	struct tm timeinfo;
    	time(&now);
    	localtime_r(&now, &timeinfo);
		
    	char strftime_buf[64];
    	strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);

        if (timeinfo.tm_hour == 3 && timeinfo.tm_min == 0) {  // 3:00 AM
            ota_update();
        }
		vTaskDelay(1 * 1000 / portTICK_PERIOD_MS);    
	}
	vTaskDelete(NULL);
}


esp_err_t esp_mesh_comm_mqtt_task_start(void)
{
    static bool is_comm_mqtt_task_started = false;
	
    s_route_table_lock = xSemaphoreCreateMutex();
    s_traffic_button_lock = xSemaphoreCreateMutex();
    
    obtain_time();
    mqtt_app_start();

    if (!is_comm_mqtt_task_started) {
		xTaskCreate(traffic_light_control, "traffic light control", 4096, NULL, 8, NULL);
        xTaskCreate(check_button, "check button task", 3072, NULL, 20, NULL);
        xTaskCreate(check_movement_sensor, "check movement task", 3072, NULL, 19, NULL);
        xTaskCreate(ota_task, "ota update", 3072, NULL, 1, NULL);
        is_comm_mqtt_task_started = true;
    }
    return ESP_OK;
}

void mesh_event_handler(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    mesh_addr_t id = {0,};
    static uint8_t last_layer = 0;

    switch (event_id) {
    case MESH_EVENT_STARTED: {
        esp_mesh_get_id(&id);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_MESH_STARTED>ID:"MACSTR"", MAC2STR(id.addr));
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOPPED>");
        mesh_layer = esp_mesh_get_layer();
    }
    break;
    case MESH_EVENT_CHILD_CONNECTED: {
        mesh_event_child_connected_t *child_connected = (mesh_event_child_connected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_CONNECTED>aid:%d, "MACSTR"",
                 child_connected->aid,
                 MAC2STR(child_connected->mac));
    }
    break;
    case MESH_EVENT_CHILD_DISCONNECTED: {
        mesh_event_child_disconnected_t *child_disconnected = (mesh_event_child_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHILD_DISCONNECTED>aid:%d, "MACSTR"",
                 child_disconnected->aid,
                 MAC2STR(child_disconnected->mac));
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_ADD: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_ADD>add %d, new:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new);
    }
    break;
    case MESH_EVENT_ROUTING_TABLE_REMOVE: {
        mesh_event_routing_table_change_t *routing_table = (mesh_event_routing_table_change_t *)event_data;
        ESP_LOGW(MESH_TAG, "<MESH_EVENT_ROUTING_TABLE_REMOVE>remove %d, new:%d",
                 routing_table->rt_size_change,
                 routing_table->rt_size_new);
    }
    break;
    case MESH_EVENT_NO_PARENT_FOUND: {
        mesh_event_no_parent_found_t *no_parent = (mesh_event_no_parent_found_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NO_PARENT_FOUND>scan times:%d",
                 no_parent->scan_times);
    }
    /* TODO handler for the failure */
    break;
    case MESH_EVENT_PARENT_CONNECTED: {
        mesh_event_connected_t *connected = (mesh_event_connected_t *)event_data;
        esp_mesh_get_id(&id);
        mesh_layer = connected->self_layer;
        memcpy(&mesh_parent_addr.addr, connected->connected.bssid, 6);
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_CONNECTED>layer:%d-->%d, parent:"MACSTR"%s, ID:"MACSTR"",
                 last_layer, mesh_layer, MAC2STR(mesh_parent_addr.addr),
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "", MAC2STR(id.addr));
        last_layer = mesh_layer;
        mesh_netifs_start(esp_mesh_is_root());
        //ESP_ERROR_CHECK(esp_mesh_set_self_organized(true, false));
        //esp_mesh_fix_root(true);
    }
    break;
    case MESH_EVENT_PARENT_DISCONNECTED: {
        mesh_event_disconnected_t *disconnected = (mesh_event_disconnected_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_PARENT_DISCONNECTED>reason:%d",
                 disconnected->reason);
        mesh_layer = esp_mesh_get_layer();
        mesh_netifs_stop();
    }
    break;
    case MESH_EVENT_LAYER_CHANGE: {
        mesh_event_layer_change_t *layer_change = (mesh_event_layer_change_t *)event_data;
        mesh_layer = layer_change->new_layer;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_LAYER_CHANGE>layer:%d-->%d%s",
                 last_layer, mesh_layer,
                 esp_mesh_is_root() ? "<ROOT>" :
                 (mesh_layer == 2) ? "<layer2>" : "");
        last_layer = mesh_layer;
    }
    break;
    case MESH_EVENT_ROOT_ADDRESS: {
        mesh_event_root_address_t *root_addr = (mesh_event_root_address_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_ADDRESS>root address:"MACSTR"",
                 MAC2STR(root_addr->addr));
    }
    break;
    case MESH_EVENT_VOTE_STARTED: {
        mesh_event_vote_started_t *vote_started = (mesh_event_vote_started_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_VOTE_STARTED>attempts:%d, reason:%d, rc_addr:"MACSTR"",
                 vote_started->attempts,
                 vote_started->reason,
                 MAC2STR(vote_started->rc_addr.addr));
    }
    break;
    case MESH_EVENT_VOTE_STOPPED: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_VOTE_STOPPED>");
        break;
    }
    case MESH_EVENT_ROOT_SWITCH_REQ: {
        mesh_event_root_switch_req_t *switch_req = (mesh_event_root_switch_req_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_SWITCH_REQ>reason:%d, rc_addr:"MACSTR"",
                 switch_req->reason,
                 MAC2STR( switch_req->rc_addr.addr));
    }
    break;
    case MESH_EVENT_ROOT_SWITCH_ACK: {
        /* new root */
        mesh_layer = esp_mesh_get_layer();
        esp_mesh_get_parent_bssid(&mesh_parent_addr);
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_SWITCH_ACK>layer:%d, parent:"MACSTR"", mesh_layer, MAC2STR(mesh_parent_addr.addr));
    }
    break;
    case MESH_EVENT_TODS_STATE: {
        mesh_event_toDS_state_t *toDs_state = (mesh_event_toDS_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_TODS_REACHABLE>state:%d", *toDs_state);
    }
    break;
    case MESH_EVENT_ROOT_FIXED: {
        mesh_event_root_fixed_t *root_fixed = (mesh_event_root_fixed_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROOT_FIXED>%s",
                 root_fixed->is_fixed ? "fixed" : "not fixed");
    }
    break;
    case MESH_EVENT_ROOT_ASKED_YIELD: {
        mesh_event_root_conflict_t *root_conflict = (mesh_event_root_conflict_t *)event_data;
        ESP_LOGI(MESH_TAG,
                 "<MESH_EVENT_ROOT_ASKED_YIELD>"MACSTR", rssi:%d, capacity:%d",
                 MAC2STR(root_conflict->addr),
                 root_conflict->rssi,
                 root_conflict->capacity);
    }
    break;
    case MESH_EVENT_CHANNEL_SWITCH: {
        mesh_event_channel_switch_t *channel_switch = (mesh_event_channel_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_CHANNEL_SWITCH>new channel:%d", channel_switch->channel);
    }
    break;
    case MESH_EVENT_SCAN_DONE: {
        mesh_event_scan_done_t *scan_done = (mesh_event_scan_done_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_SCAN_DONE>number:%d",
                 scan_done->number);
    }
    break;
    case MESH_EVENT_NETWORK_STATE: {
        mesh_event_network_state_t *network_state = (mesh_event_network_state_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_NETWORK_STATE>is_rootless:%d",
                 network_state->is_rootless);
    }
    break;
    case MESH_EVENT_STOP_RECONNECTION: {
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_STOP_RECONNECTION>");
    }
    break;
    case MESH_EVENT_FIND_NETWORK: {
        mesh_event_find_network_t *find_network = (mesh_event_find_network_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_FIND_NETWORK>new channel:%d, router BSSID:"MACSTR"",
                 find_network->channel, MAC2STR(find_network->router_bssid));
    }
    break;
    case MESH_EVENT_ROUTER_SWITCH: {
        mesh_event_router_switch_t *router_switch = (mesh_event_router_switch_t *)event_data;
        ESP_LOGI(MESH_TAG, "<MESH_EVENT_ROUTER_SWITCH>new router:%s, channel:%d, "MACSTR"",
                 router_switch->ssid, router_switch->channel, MAC2STR(router_switch->bssid));
    }
    break;
    default:
        ESP_LOGI(MESH_TAG, "unknown id:%" PRId32 "", event_id);
        break;
    }
}

void ip_event_handler(void *arg, esp_event_base_t event_base,
                      int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    ESP_LOGI(MESH_TAG, "<IP_EVENT_STA_GOT_IP>IP:" IPSTR, IP2STR(&event->ip_info.ip));
    s_current_ip.addr = event->ip_info.ip.addr;
#if !CONFIG_MESH_USE_GLOBAL_DNS_IP
    esp_netif_t *netif = event->esp_netif;
    esp_netif_dns_info_t dns;
    ESP_ERROR_CHECK(esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
    mesh_netif_start_root_ap(esp_mesh_is_root(), dns.ip.u_addr.ip4.addr);
#endif
    esp_mesh_comm_mqtt_task_start();
}


void app_main(void)
{
	ESP_ERROR_CHECK(traffic_light_init());
    ESP_ERROR_CHECK(nvs_flash_init());
    /*  tcpip initialization */
    ESP_ERROR_CHECK(esp_netif_init());
    /*  event initialization */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /*  crete network interfaces for mesh (only station instance saved for further manipulation, soft AP instance ignored */
    ESP_ERROR_CHECK(mesh_netifs_init(recv_cb));

    /*  wifi initialization */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());
    /*  mesh initialization */
    ESP_ERROR_CHECK(esp_mesh_init());
    ESP_ERROR_CHECK(esp_event_handler_register(MESH_EVENT, ESP_EVENT_ANY_ID, &mesh_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mesh_set_max_layer(CONFIG_MESH_MAX_LAYER));
    ESP_ERROR_CHECK(esp_mesh_set_vote_percentage(1));
    ESP_ERROR_CHECK(esp_mesh_set_ap_assoc_expire(10));
    mesh_cfg_t cfg = MESH_INIT_CONFIG_DEFAULT();
    /* mesh ID */
    memcpy((uint8_t *) &cfg.mesh_id, MESH_ID, 6);
    /* router */
    cfg.channel = CONFIG_MESH_CHANNEL;
    cfg.router.ssid_len = strlen(CONFIG_MESH_ROUTER_SSID);
    memcpy((uint8_t *) &cfg.router.ssid, CONFIG_MESH_ROUTER_SSID, cfg.router.ssid_len);
    memcpy((uint8_t *) &cfg.router.password, CONFIG_MESH_ROUTER_PASSWD,
           strlen(CONFIG_MESH_ROUTER_PASSWD));
    /* mesh softAP */
    ESP_ERROR_CHECK(esp_mesh_set_ap_authmode(CONFIG_MESH_AP_AUTHMODE));
    cfg.mesh_ap.max_connection = CONFIG_MESH_AP_CONNECTIONS;
    cfg.mesh_ap.nonmesh_max_connection = CONFIG_MESH_NON_MESH_AP_CONNECTIONS;
    memcpy((uint8_t *) &cfg.mesh_ap.password, CONFIG_MESH_AP_PASSWD,
           strlen(CONFIG_MESH_AP_PASSWD));
      
    ESP_ERROR_CHECK(esp_mesh_set_config(&cfg));
    
    /* mesh start */
    ESP_ERROR_CHECK(esp_mesh_start());
    
    ESP_LOGI(MESH_TAG, "mesh starts successfully, heap:%" PRId32 ", %s",  esp_get_free_heap_size(),
             esp_mesh_is_root_fixed() ? "root fixed" : "root not fixed");
}
