/*************************************************************************
*
* Copyright 2020 highstreet technologies GmbH and others
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
***************************************************************************/

#define _GNU_SOURCE

#include "ves_pnf_registration.h"
#include "utils/log_utils.h"
#include "utils/sys_utils.h"
#include "utils/rand_utils.h"
#include "utils/http_client.h"
#include "utils/nts_utils.h"
#include <stdio.h>
#include <assert.h>

#include "core/session.h"
#include "core/framework.h"

#define PNF_REGISTRATION_SCHEMA_XPATH               "/nts-network-function:simulation/network-function/ves/pnf-registration"

static int ves_pnf_sequence_number = 0;

static int ves_pnf_registration_send(sr_session_ctx_t *current_session, const char *nf_ip_address, int nf_port, bool is_tls);
static cJSON* ves_create_pnf_registration_fields(const char *nf_ip_address, int nf_port, bool is_tls);

int ves_pnf_registration_feature_start(sr_session_ctx_t *current_session) {
    assert(current_session);

    ves_pnf_sequence_number = 0;

    sr_val_t *value = 0;
    int rc = NTS_ERR_OK;
    bool pnf_registration_enabled = false;
    rc = sr_get_item(current_session, PNF_REGISTRATION_SCHEMA_XPATH, 0, &value);
    if(rc == SR_ERR_OK) {
        pnf_registration_enabled = value->data.bool_val;
        sr_free_val(value);
    }
    else if(rc != SR_ERR_NOT_FOUND) {
        log_error("sr_get_item failed");
        return NTS_ERR_FAILED;
    }

    if(pnf_registration_enabled == false) {
        log_message(2, "PNF registration is disabled\n");
        return NTS_ERR_OK;
    }

    bool host_addressing_enabled = (nts_mount_point_addressing_method_get(current_session) == HOST_MAPPING);

    char nf_ip_address[128];
    int nf_port;

    if(host_addressing_enabled) {
        strcpy(nf_ip_address, framework_environment.host_ip);
        nf_port = framework_environment.host_base_port;
    }
    else {
        if(framework_environment.ip_v6_enabled) {
            strcpy(nf_ip_address, framework_environment.ip_v6);
        }
        else {
            strcpy(nf_ip_address, framework_environment.ip_v4);
        } 
        
        nf_port = STANDARD_NETCONF_PORT;
    }

    int port = 0;
    for(int i = 0; i < framework_environment.ssh_connections; ++port, ++i) {
        rc = ves_pnf_registration_send(current_session, nf_ip_address, nf_port + port, false);
        if(rc != NTS_ERR_OK) {
            log_error("could not send pnfRegistration message for IP=%s and port=%d and protocol SSH", nf_ip_address, nf_port + port);
            continue;
        }
    }

    for(int i = 0; i < framework_environment.tls_connections; ++port, ++i) {
        rc = ves_pnf_registration_send(current_session, nf_ip_address, nf_port + port, true);
        if(rc != NTS_ERR_OK) {
            log_error("could not send pnfRegistration message for IP=%s and port=%d and protocol TLS", nf_ip_address, nf_port + port);
            continue;
        }
    }

    return NTS_ERR_OK;
}


static int ves_pnf_registration_send(sr_session_ctx_t *current_session, const char *nf_ip_address, int nf_port, bool is_tls) {
    assert(current_session);
    assert(nf_ip_address);

    cJSON *post_data_json = cJSON_CreateObject();
    if(post_data_json == 0) {
        log_error("could not create cJSON object");
        return NTS_ERR_FAILED;
    }

    cJSON *event = cJSON_CreateObject();
    if(event == 0) {
        log_error("could not create cJSON object");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }
    
    if(cJSON_AddItemToObject(post_data_json, "event", event) == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }

    char *hostname_string = framework_environment.hostname;
    char source_name[100];
	sprintf(source_name, "%s_%d", hostname_string, nf_port);

    cJSON *common_event_header = ves_create_common_event_header("pnfRegistration", "EventType5G", source_name, "Normal", ves_pnf_sequence_number++);
    if(common_event_header == 0) {
        log_error("could not create cJSON object");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }
    
    if(cJSON_AddItemToObject(event, "commonEventHeader", common_event_header) == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }

	cJSON *pnf_registration_fields = ves_create_pnf_registration_fields(nf_ip_address, nf_port, is_tls);
    if(pnf_registration_fields == 0) {
        log_error("could not create cJSON object");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }
    
    if(cJSON_AddItemToObject(event, "pnfRegistrationFields", pnf_registration_fields) == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(post_data_json);
        return NTS_ERR_FAILED;
    }

    char *post_data = cJSON_PrintUnformatted(post_data_json);
    cJSON_Delete(post_data_json);
    if(post_data == 0) {
        log_error("cJSON_PrintUnformatted failed");
        return NTS_ERR_FAILED;
    }


    ves_details_t *ves_details = ves_endpoint_details_get(current_session);
    if(!ves_details) {
        log_error("ves_endpoint_details_get failed");
        free(post_data);
        return NTS_ERR_FAILED;
    }
    
    int rc = http_request(ves_details->url, ves_details->username, ves_details->password, "POST", post_data, 0, 0);
    ves_details_free(ves_details);
    free(post_data);
    
    if(rc != NTS_ERR_OK) {
        log_error("http_request failed");
        return NTS_ERR_FAILED;
    }

    return NTS_ERR_OK;
}

static cJSON* ves_create_pnf_registration_fields(const char *nf_ip_address, int nf_port, bool is_tls) {
    assert(nf_ip_address);

    //checkAL aici n-ar trebui niste valori "adevarate" ?

    cJSON *pnf_registration_fields = cJSON_CreateObject();
    if(pnf_registration_fields == 0) {
        log_error("could not create JSON object");
        return 0;
    }

    if(cJSON_AddStringToObject(pnf_registration_fields, "pnfRegistrationFieldsVersion", "2.0") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(pnf_registration_fields, "lastServiceDate", "2019-08-16") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    char *mac_addr = rand_mac_address();
    if(mac_addr == 0) {
        log_error("rand_mac_address failed")
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(pnf_registration_fields, "macAddress", mac_addr) == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        free(mac_addr);
        return 0;
    }
    free(mac_addr);

    if(cJSON_AddStringToObject(pnf_registration_fields, "manufactureDate", "2019-08-16") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(pnf_registration_fields, "modelNumber", "Simulated Device Melacon") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(pnf_registration_fields, "oamV4IpAddress", nf_ip_address) == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(pnf_registration_fields, "oamV6IpAddress", "0:0:0:0:0:ffff:a0a:011") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    char serial_number[512];
    sprintf(serial_number, "%s-%s-%d-Simulated Device Melacon", framework_environment.hostname, nf_ip_address, nf_port);

    if(cJSON_AddStringToObject(pnf_registration_fields, "serialNumber", serial_number) == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(pnf_registration_fields, "softwareVersion", "2.3.5") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(pnf_registration_fields, "unitFamily", "Simulated Device") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(pnf_registration_fields, "unitType", "O-RAN-sim") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(pnf_registration_fields, "vendorName", "Melacon") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    cJSON *additional_fields = cJSON_CreateObject();
    if(additional_fields == 0) {
        log_error("could not create JSON object");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }
    cJSON_AddItemToObject(pnf_registration_fields, "additionalFields", additional_fields);

    char port_string[10];
    sprintf(port_string, "%d", nf_port);

    if(cJSON_AddStringToObject(additional_fields, "oamPort", port_string) == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(is_tls) {
        //TLS specific configuration
        if(cJSON_AddStringToObject(additional_fields, "protocol", "TLS") == 0) {
            log_error("cJSON_AddItemToObject failed");
            cJSON_Delete(pnf_registration_fields);
            return 0;
        }

        if(cJSON_AddStringToObject(additional_fields, "username", "netconf") == 0) {
            log_error("cJSON_AddItemToObject failed");
            cJSON_Delete(pnf_registration_fields);
            return 0;
        }

        if(cJSON_AddStringToObject(additional_fields, "keyId", KS_KEY_NAME) == 0) {
            log_error("cJSON_AddItemToObject failed");
            cJSON_Delete(pnf_registration_fields);
            return 0;
        }
    }
    else {
        //SSH specific configuration
        if(cJSON_AddStringToObject(additional_fields, "protocol", "SSH") == 0) {
            log_error("cJSON_AddItemToObject failed");
            cJSON_Delete(pnf_registration_fields);
            return 0;
        }

        if(cJSON_AddStringToObject(additional_fields, "username", "netconf") == 0) {
            log_error("cJSON_AddItemToObject failed");
            cJSON_Delete(pnf_registration_fields);
            return 0;
        }

        if(cJSON_AddStringToObject(additional_fields, "password", "netconf") == 0) {
            log_error("cJSON_AddItemToObject failed");
            cJSON_Delete(pnf_registration_fields);
            return 0;
        }
    }

    if(cJSON_AddStringToObject(additional_fields, "reconnectOnChangedSchema", "false") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(additional_fields, "sleep-factor", "1.5") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(additional_fields, "tcpOnly", "false") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(additional_fields, "connectionTimeout", "20000") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(additional_fields, "maxConnectionAttempts", "100") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(additional_fields, "betweenAttemptsTimeout", "2000") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    if(cJSON_AddStringToObject(additional_fields, "keepaliveDelay", "120") == 0) {
        log_error("cJSON_AddItemToObject failed");
        cJSON_Delete(pnf_registration_fields);
        return 0;
    }

    return pnf_registration_fields;
}
