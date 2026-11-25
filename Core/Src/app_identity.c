#include "app_identity.h"

#include "app_gateway_platform.h"
#include "debug_uart.h"
#include "main.h"

#include <stdio.h>
#include <string.h>

/* How long after a configuration gives the unit its deviceId before it restarts: the deployment acknowledgement must be
 * published on the old topic first (a deploy takes about 8 s on this modem). */
#define APP_ID_RESTART_DELAY_MS 12000UL

static W25Q_Handle *s_flash;
static char s_record[APP_ID_LEN + 1U];  /* from the manufacture record */
static char s_config[APP_ID_LEN + 1U];  /* device_id of the applied configuration */
static char s_topic[40];                /* what the topics were built with ("" until they are) */
static char s_imei_seen[20];
static uint32_t s_seq;                  /* sequence of the newest valid record copy */
static uint8_t  s_slot_b_newest;
static uint8_t  s_have_record;
static uint8_t  s_restart_pending;
static uint32_t s_restart_at;

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
  if (!va && !vb) return;
  const app_id_record_t *best = (va && (!vb || a.seq >= b.seq)) ? &a : &b;
  s_slot_b_newest = (best == &b) ? 1U : 0U;
  s_seq = best->seq;
  memcpy(s_record, best->device_id, APP_ID_LEN);
  s_record[APP_ID_LEN] = '\0';
  s_have_record = 1U;
  print("[ID] manufacture record found: deviceId=%s\r\n", s_record);
}

static const char *chosen(const char *imei, app_id_src_t *src)
{
  return app_id_choose(s_have_record ? s_record : NULL, s_config, imei, src);
}

const char *app_identity_topic_id(const char *imei)
{
  if (imei) { strncpy(s_imei_seen, imei, sizeof(s_imei_seen) - 1U); s_imei_seen[sizeof(s_imei_seen) - 1U] = '\0'; }
  app_id_src_t src;
  const char *id = chosen(imei, &src);
  if (!id) return NULL;
  strncpy(s_topic, id, sizeof(s_topic) - 1U);
  s_topic[sizeof(s_topic) - 1U] = '\0';
  print("[ID] topics use %s (source=%s)\r\n", s_topic, app_id_src_name(src));
  return s_topic;
}

void app_identity_client_id(char *out, size_t n, const char *imei)
{
  app_id_src_t src;
  const char *id = chosen(imei, &src);
  if (!id) { snprintf(out, n, "STM32_unknown"); return; }
  if (src == APP_ID_SRC_IMEI) snprintf(out, n, "STM32_%s", id); /* unchanged for a unit still on its IMEI */
  else snprintf(out, n, "%s", id);
}

const char *app_identity_device_id(void)
{
  if (s_have_record) return s_record;
  return s_config;
}

app_id_src_t app_identity_source(const char *imei)
{
  app_id_src_t src;
  (void)chosen(imei, &src);
  return src;
}

static void schedule_restart_if_topic_changes(uint32_t delay_ms)
{
  if (s_restart_pending || !s_topic[0]) return;               /* topics not built yet (boot): nothing to change */
  const char *id = chosen(s_imei_seen, NULL);
  if (id && strcmp(id, s_topic) != 0) {
    s_restart_pending = 1U;
    s_restart_at = HAL_GetTick() + delay_ms;
    print("[ID] topic id changes %s -> %s: restarting in %lu s\r\n", s_topic, id, (unsigned long)(delay_ms / 1000UL));
  }
}

void app_identity_note_config(const char *device_id)
{
  if (!app_id_is_device_id(device_id)) return;
  if (strcmp(s_config, device_id) == 0) return;
  memcpy(s_config, device_id, APP_ID_LEN);
  s_config[APP_ID_LEN] = '\0';
  schedule_restart_if_topic_changes(APP_ID_RESTART_DELAY_MS);
}

void app_identity_hardware_ids_json(char *out, size_t n, const char *imei)
{
  uint32_t w[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
  char uid[APP_ID_LEN + 1U];
  app_id_format_uid(w, uid);
  if (imei && imei[0]) snprintf(out, n, "{\"imei\":\"%s\",\"mcuUid\":\"%s\"}", imei, uid);
  else snprintf(out, n, "{\"mcuUid\":\"%s\"}", uid);
}

/* ---- provisioning (USB, at manufacture) ---- */
static uint8_t write_record(const char *device_id)
{
  if (!s_flash) return 0U;
  app_id_record_t r;
  app_id_record_pack(&r, device_id, s_have_record ? s_seq + 1U : 1U);
  uint32_t addr = (s_have_record && !s_slot_b_newest) ? APP_ID_SLOT_B_ADDR : APP_ID_SLOT_A_ADDR;
  if (!w25q_erase_sector_4k(s_flash, addr)) return 0U;
  if (!w25q_write(s_flash, addr, (const uint8_t *)&r, sizeof(r))) return 0U;
  app_id_record_t back;
  if (!read_slot(addr, &back) || strcmp(back.device_id, device_id) != 0) return 0U;
  return 1U;
}

static uint8_t clear_records(void)
{
  if (!s_flash) return 0U;
  uint8_t ok = w25q_erase_sector_4k(s_flash, APP_ID_SLOT_A_ADDR) ? 1U : 0U;
  ok &= w25q_erase_sector_4k(s_flash, APP_ID_SLOT_B_ADDR) ? 1U : 0U;
  return ok;
}

static void print_status(void)
{
  app_id_src_t src;
  const char *id = chosen(s_imei_seen, &src);
  uint32_t w[3] = { HAL_GetUIDw0(), HAL_GetUIDw1(), HAL_GetUIDw2() };
  char uid[APP_ID_LEN + 1U];
  app_id_format_uid(w, uid);
  print("[ID] deviceId=%s source=%s record=%s config=%s imei=%s mcuUid=%s\r\n",
        id ? id : "(none)", app_id_src_name(src), s_have_record ? s_record : "(none)", s_config[0] ? s_config : "(none)",
        s_imei_seen[0] ? s_imei_seen : "(none)", uid);
}

static void run_command(char *line)
{
  if (strcmp(line, "ID") == 0 || strcmp(line, "ID?") == 0) { print_status(); return; }
  if (strcmp(line, "ID CLEAR") == 0) {
    if (clear_records()) { s_have_record = 0U; s_record[0] = '\0'; print("[ID] record cleared\r\n"); schedule_restart_if_topic_changes(1000UL); }
    else print("[ID][ERR] clear failed\r\n");
    return;
  }
  if (strncmp(line, "ID SET ", 7) == 0) {
    const char *id = line + 7;
    if (!app_id_is_device_id(id)) { print("[ID][ERR] not a deviceId (24 lowercase hex characters)\r\n"); return; }
    if (s_have_record && strcmp(s_record, id) != 0) { print("[ID][ERR] a different record exists: ID CLEAR first\r\n"); return; }
    if (s_have_record) { print("[ID] already set to %s\r\n", id); return; }
    if (!write_record(id)) { print("[ID][ERR] could not write the record\r\n"); return; }
    memcpy(s_record, id, APP_ID_LEN);
    s_record[APP_ID_LEN] = '\0';
    s_have_record = 1U;
    s_seq = 1U;
    s_slot_b_newest = 0U;
    print("[ID] record written: deviceId=%s\r\n", s_record);
    schedule_restart_if_topic_changes(1000UL);
    return;
  }
  print("[ID][ERR] unknown command (ID, ID SET <deviceId>, ID CLEAR)\r\n");
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
  if (s_restart_pending && (int32_t)(now_ms - s_restart_at) >= 0) {
    s_restart_pending = 0U;
    print("[ID] restarting to use the new id\r\n");
    app_gateway_set_reset_intent(APP_RESET_INTENT_IDENTITY);
    HAL_Delay(200U);
    NVIC_SystemReset();
  }
}
