#pragma once
#include <stddef.h>
#include "nds.h"
#define WFLAG_APDATA_WEP 2
#define WFLAG_APDATA_WPA 4
#define WFLAG_APDATA_COMPATIBLE 8
#define WFLAG_APDATA_CONFIG_IN_WFC 0x80
#define WIFIGETDATA_RSSI 2
#define INIT_ONLY 0
#define WIFI_ATTEMPT_DSI_MODE 8
enum { ASSOCSTATUS_DISCONNECTED, ASSOCSTATUS_SEARCHING, ASSOCSTATUS_AUTHENTICATING, ASSOCSTATUS_ASSOCIATING, ASSOCSTATUS_ACQUIRINGDHCP, ASSOCSTATUS_ASSOCIATED, ASSOCSTATUS_CANNOTCONNECT };
typedef struct { char ssid[33]; u8 ssid_len; u8 bssid[6]; u8 channel; s16 rssi; u16 flags; } Wifi_AccessPoint;
#ifdef __cplusplus
extern "C" {
#endif
bool Wifi_InitDefault(unsigned flags); bool Wifi_Deinit(void); void Wifi_DisableWifi(void); void Wifi_EnableWifi(void);
void Wifi_ScanMode(void); int Wifi_GetNumAP(void); int Wifi_GetAPData(int, Wifi_AccessPoint*);
int Wifi_ConnectSecureAP(Wifi_AccessPoint*, const void*, size_t); int Wifi_ConnectWfcAP(Wifi_AccessPoint*);
int Wifi_AssocStatus(void); int Wifi_DisconnectAP(void); u32 Wifi_GetIP(void); int Wifi_GetData(int, int, unsigned char*);
void Wifi_SetIP(u32, u32, u32, u32, u32);
#ifdef __cplusplus
}
#endif
