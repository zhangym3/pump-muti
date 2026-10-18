#pragma once
#include "esp_ble_mesh_defs.h"
namespace pump {
bool managementInit(uint16_t primary);
void managementProvisionEvent(esp_ble_mesh_prov_cb_event_t event, esp_ble_mesh_prov_cb_param_t *p);
}
