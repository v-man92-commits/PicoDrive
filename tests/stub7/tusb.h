#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { int role; int speed; } tusb_rhport_init_t;
enum { TUSB_ROLE_DEVICE = 1, TUSB_SPEED_AUTO = 0 };
enum { SCSI_SENSE_NONE = 0, SCSI_SENSE_NOT_READY = 2, SCSI_SENSE_MEDIUM_ERROR = 3, SCSI_SENSE_ILLEGAL_REQUEST = 5, SCSI_SENSE_DATA_PROTECT = 7, SCSI_SENSE_UNIT_ATTENTION = 6 };
typedef struct __attribute__((packed)) {
    uint8_t peripheral_device_type : 5; uint8_t peripheral_qualifier : 3;
    uint8_t reserved : 7; uint8_t is_removable : 1;
    uint8_t version; uint8_t response_data_format : 4; uint8_t hierarchical_support : 1; uint8_t normal_aca : 1; uint8_t reserved2 : 2;
    uint8_t additional_length; uint8_t protect : 1; uint8_t reserved3 : 2; uint8_t third_party_copy : 1; uint8_t target_port_group_support : 2; uint8_t access_control_coordinator : 1; uint8_t scc_support : 1;
    uint8_t addr16 : 1; uint8_t reserved4 : 3; uint8_t multi_port : 1; uint8_t reserved5 : 1; uint8_t enclosure_service : 1; uint8_t reserved6 : 1;
    uint8_t reserved7 : 1; uint8_t command_que : 1; uint8_t reserved8 : 2; uint8_t sync : 1; uint8_t wbus16 : 1; uint8_t reserved9 : 2;
    uint8_t vendor_id[8]; uint8_t product_id[16]; uint8_t product_rev[4];
} scsi_inquiry_resp_t;
bool tusb_init(uint8_t, const tusb_rhport_init_t*);
void tud_task(void);
bool tud_connect(void);
bool tud_disconnect(void);
bool tud_suspended(void);
bool tud_mounted(void);
bool tud_msc_set_sense(uint8_t lun, uint8_t key, uint8_t asc, uint8_t ascq);
// callbacks implemented by ImageDisk.cpp
void tud_msc_inquiry_cb(uint8_t, uint8_t[8], uint8_t[16], uint8_t[4]);
bool tud_msc_test_unit_ready_cb(uint8_t);
void tud_msc_capacity_cb(uint8_t, uint32_t*, uint16_t*);
bool tud_msc_start_stop_cb(uint8_t, uint8_t, bool, bool);
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t, uint8_t, uint8_t);
bool tud_msc_is_writable_cb(uint8_t);
int32_t tud_msc_read10_cb(uint8_t, uint32_t, uint32_t, void*, uint32_t);
int32_t tud_msc_write10_cb(uint8_t, uint32_t, uint32_t, uint8_t*, uint32_t);
int32_t tud_msc_scsi_cb(uint8_t, uint8_t const[16], void*, uint16_t);
uint32_t tud_msc_stream_read_blocks_cb(uint8_t, uint8_t const[16], uint32_t);
void tud_msc_read_context_cb(uint8_t, uint8_t const[16]);
#ifdef __cplusplus
}
#endif
#define CFG_TUD_MSC_EP_BUFSIZE 8192
