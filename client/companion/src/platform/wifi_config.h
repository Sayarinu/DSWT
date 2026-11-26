#ifndef COMPANION_PLATFORM_WIFI_CONFIG_H
#define COMPANION_PLATFORM_WIFI_CONFIG_H

#include <stddef.h>

#include "platform/wifi_status.h"

/* The Wi-Fi network the player joined in the companion's Wi-Fi settings,
 * kept in <card>_nds/dswt/wifi.cfg so the next launch joins it by itself.
 * The password is stored as plain text, as the screen says; the file is
 * small, human-readable and parsed defensively:
 *
 *   # comment lines and blank lines are ignored
 *   version=1
 *   security=wpa2        (open, wep or wpa2)
 *   ssid=Home Network
 *   key=the passphrase
 *
 * Values run to the end of the line. A backslash escapes itself (\\) and any
 * byte below 0x20 or 0x7F as \xHH. Unknown names are ignored; a repeated
 * name, a missing ssid, a bad version or a key that does not suit the
 * security rejects the whole file. It is written to wifi.cfg.tmp, synced and
 * renamed over wifi.cfg; a .tmp left by a power cut is used only when
 * wifi.cfg itself is missing. */

#define CP_WIFI_CONFIG_PATH "_nds/dswt/wifi.cfg"
#define CP_WIFI_CONFIG_FILE_MAX 1024

typedef struct CpWifiConfig {
    unsigned security; /* enum CpWifiSecurity: open, WEP or WPA2 */
    char ssid[CP_WIFI_SSID_MAX + 1];
    char key[CP_WIFI_KEY_MAX + 1];
} CpWifiConfig;

/* Parse a file's bytes. Zero when it describes a network. */
int CpWifiConfig_Parse(const char *text, size_t size, CpWifiConfig *out);
/* The file's text for a network; returns its length, or 0 when the network
 * cannot be stored (no SSID, a key that does not suit the security). */
size_t CpWifiConfig_Format(const CpWifiConfig *, char *out, size_t size);

/* Read the network from path: 0 with one, 1 when there is no file, -1 when
 * the file is unreadable or invalid (the caller ignores it). */
int CpWifiConfig_Load(const char *path, CpWifiConfig *out);
/* Store the network at path through path.tmp. Zero once it is in place. */
int CpWifiConfig_Save(const char *path, const CpWifiConfig *);
/* Delete the stored network (and any .tmp). Zero when none is left. */
int CpWifiConfig_Forget(const char *path);

/* Overwrite memory that held a password. */
void CpWifiConfig_Wipe(void *, size_t);

#endif
