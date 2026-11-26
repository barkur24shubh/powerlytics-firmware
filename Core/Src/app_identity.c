#include "app_identity.h"

#include "debug_uart.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

static W25Q_Handle *s_flash;
static char s_record[APP_ID_LEN + 1U];  /* the stored deviceId */
static char s_config_seen[APP_ID_LEN + 1U];
static uint32_t s_seq;                  /* sequence of the newest valid record copy */
static uint8_t  s_have_record;

/* USB input: the callback only copies bytes; the runtime task parses. */
#define RX_N 96U
static volatile uint8_t s_rx[RX_N];
static volatile uint8_t s_rx_w;
static uint8_t s_rx_r;

void app_identity_set_flash(W25Q_Handle *flash) { s_flash = flash; }

static uint8_t read_slot(uint32_t addr, app_id_record_t *out)
{
  if (!s_flash || !w25q_read(s_flash, addr, (uint8_t *)out, sizeof(*out))) return 0U;
  return app_id_record_valid(out);
}

void app_identity_load(void)
{
  app_id_record_t a, b;
  uint8_t va = read_slot(APP_ID_SLOT_A_ADDR, &a);
  uint8_t vb = read_slot(APP_ID_SLOT_B_ADDR, &b);
  s_have_record = 0U;
  s_record[0] = '\0';
  if (!va && !vb) { print("[ID] NOT PROVISIONED: no deviceId stored; the unit stays off the network (ID SET <deviceId>)\r\n"); return; }
  const app_id_record_t *best = (va && (!vb || a.seq >= b.seq)) ? &a : &b;
  s_seq = best->seq;
  memcpy(s_record, best->device_id, APP_ID_LEN);
  s_record[APP_ID_LEN] = '\0';
  s_have_record = 1U;
  print("[ID] deviceId=%s (stored)\r\n", s_record);
}

const char *app_identity_device_id(void)
{
  return s_have_record ? s_record : "";
}

void app_identity_client_id(char *out, size_t n)
{
  snprintf(out, n, "%s", app_identity_device_id());
}

void app_identity_note_config(const char *device_id)
{
  if (!app_id_is_device_id(device_id)) return;
  if (strcmp(s_config_seen, device_id) == 0) return;
  memcpy(s_config_seen, device_id, APP_ID_LEN);
  s_config_seen[APP_ID_LEN] = '\0';
  if (s_have_record && !app_id_config_matches(s_record, device_id))
    print("[ID][WARN] the configuration is for %s, this unit is %s: its device_id is ignored\r\n", device_id, s_record);
}

void app_identity_hardware_ids_json(char *out, size_t n, const char *imei)
{
  uint32_t w[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
  char uid[APP_ID_LEN + 1U];
  app_id_format_uid(w, uid);
  if (imei && imei[0]) snprintf(out, n, "{\"imei\":\"%s\",\"mcuUid\":\"%s\"}", imei, uid);
  else snprintf(out, n, "{\"mcuUid\":\"%s\"}", uid);
}

/* ---- provisioning (USB, once, when the unit is programmed) ---- */
static uint8_t write_record(const char *device_id)
{
  if (!s_flash) return 0U;
  app_id_record_t r;
  app_id_record_pack(&r, device_id, 1U);
  if (!w25q_erase_sector_4k(s_flash, APP_ID_SLOT_A_ADDR)) return 0U;
  if (!w25q_write(s_flash, APP_ID_SLOT_A_ADDR, (const uint8_t *)&r, sizeof(r))) return 0U;
  app_id_record_t back;
  return (read_slot(APP_ID_SLOT_A_ADDR, &back) && strcmp(back.device_id, device_id) == 0) ? 1U : 0U;
}

static void print_status(void)
{
  uint32_t w[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
  char uid[APP_ID_LEN + 1U];
  app_id_format_uid(w, uid);
  print("[ID] deviceId=%s provisioned=%s mcuUid=%s\r\n", s_have_record ? s_record : "(none)", s_have_record ? "yes" : "NO", uid);
}

static void run_command(char *line)
{
  if (strcmp(line, "ID") == 0 || strcmp(line, "ID?") == 0) { print_status(); return; }
  if (strncmp(line, "ID SET ", 7) == 0) {
    const char *id = line + 7;
    if (!app_id_is_device_id(id)) { print("[ID][ERR] not a deviceId (24 lowercase hex characters)\r\n"); return; }
    if (s_have_record) {
      if (strcmp(s_record, id) == 0) print("[ID] already set to %s\r\n", id);
      else print("[ID][ERR] this unit already has a deviceId and it is permanent\r\n");
      return;
    }
    if (!write_record(id)) { print("[ID][ERR] could not write the record\r\n"); return; }
    memcpy(s_record, id, APP_ID_LEN);
    s_record[APP_ID_LEN] = '\0';
    s_have_record = 1U;
    s_seq = 1U;
    print("[ID] record written: deviceId=%s (permanent). Restart the unit to connect.\r\n", s_record);
    return;
  }
  print("[ID][ERR] unknown command (ID, ID SET <deviceId>)\r\n");
}

void app_identity_usb_rx(const uint8_t *buf, uint32_t len)
{
  for (uint32_t i = 0U; i < len; i++) {
    uint8_t next = (uint8_t)((s_rx_w + 1U) % RX_N);
    if (next == s_rx_r) return; /* full: drop; the sender retries */
    s_rx[s_rx_w] = buf[i];
    s_rx_w = next;
  }
}

void app_identity_service(uint32_t now_ms)
{
  (void)now_ms;
  static char line[72];
  static uint8_t n;
  while (s_rx_r != s_rx_w) {
    char c = (char)s_rx[s_rx_r];
    s_rx_r = (uint8_t)((s_rx_r + 1U) % RX_N);
    if (c == '\r' || c == '\n') {
      if (n) { line[n] = '\0'; run_command(line); n = 0U; }
    } else if (n < sizeof(line) - 1U) {
      line[n++] = c;
    } else {
      n = 0U; /* too long: discard */
    }
  }
}
