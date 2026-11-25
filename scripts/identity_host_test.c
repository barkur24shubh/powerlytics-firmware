/* Host test for Core/Src/app_identity_core.c.
 *   cc -std=c11 -Wall -Wextra -ICore/Inc -o /tmp/identity_host_test scripts/identity_host_test.c Core/Src/app_identity_core.c
 */
#include <stdio.h>
#include <string.h>
#include "app_identity_core.h"

static int fails, checks;
#define CHECK(cond, msg) do { checks++; if (!(cond)) { fails++; printf("  FAIL: %s\n", msg); } } while (0)

static const char *ID_A = "6af82bffd57ed01438393b29";
static const char *ID_B = "6b0701f94e58a3d94eb73b5b";

int main(void)
{
  printf("I1 what a deviceId looks like\n");
  CHECK(app_id_is_device_id(ID_A), "24 lowercase hex is a deviceId");
  CHECK(!app_id_is_device_id("6AF82BFFD57ED01438393B29"), "uppercase is not (the platform writes lowercase)");
  CHECK(!app_id_is_device_id("6af82bffd57ed01438393b2"), "23 characters is not");
  CHECK(!app_id_is_device_id("6af82bffd57ed01438393b290"), "25 characters is not");
  CHECK(!app_id_is_device_id("863110085107293"), "an IMEI is not");
  CHECK(!app_id_is_device_id("6af82bffd57ed01438393bzz"), "non-hex is not");
  CHECK(!app_id_is_device_id(""), "empty is not");
  CHECK(!app_id_is_device_id(NULL), "NULL is not");

  printf("I2 the order of precedence: record, then config, then IMEI\n");
  app_id_src_t src;
  CHECK(app_id_choose(ID_A, ID_B, "863110085107293", &src) == ID_A && src == APP_ID_SRC_RECORD, "the manufacture record wins");
  CHECK(app_id_choose("", ID_B, "863110085107293", &src) == ID_B && src == APP_ID_SRC_CONFIG, "then the deployed configuration's device_id");
  CHECK(strcmp(app_id_choose(NULL, "", "863110085107293", &src), "863110085107293") == 0 && src == APP_ID_SRC_IMEI, "a unit that knows only its IMEI uses it for first contact");
  CHECK(app_id_choose("", "", "", &src) == NULL && src == APP_ID_SRC_NONE, "a unit that knows nothing has no id (it must not publish under a made-up one)");
  CHECK(app_id_choose("garbage", "also-garbage", NULL, &src) == NULL, "malformed ids are ignored, never used");
  CHECK(app_id_choose("not-an-id", ID_B, NULL, &src) == ID_B && src == APP_ID_SRC_CONFIG, "a malformed record falls through to the config");
  CHECK(strcmp(app_id_src_name(APP_ID_SRC_RECORD), "record") == 0 && strcmp(app_id_src_name(APP_ID_SRC_NONE), "none") == 0, "source names");

  printf("I3 the MCU factory unique ID as 24 hex characters\n");
  const uint32_t uid[3] = { 0x0047001EU, 0x3532510DU, 0x31353637U };
  char out[APP_ID_LEN + 1U];
  app_id_format_uid(uid, out);
  CHECK(strcmp(out, "0047001E3532510D31353637") == 0, "three words, big-endian hex, uppercase, fixed width");
  const uint32_t zero[3] = { 0, 0, 1 };
  app_id_format_uid(zero, out);
  CHECK(strcmp(out, "000000000000000000000001") == 0, "leading zeros are kept");

  printf("I4 the record kept in flash\n");
  app_id_record_t r;
  app_id_record_pack(&r, ID_A, 7U);
  CHECK(app_id_record_valid(&r), "a packed record is valid");
  CHECK(r.seq == 7U && strcmp(r.device_id, ID_A) == 0, "it holds the id and the sequence");
  app_id_record_t bad = r; bad.device_id[3] ^= 1;
  CHECK(!app_id_record_valid(&bad), "a flipped bit is caught by the crc");
  bad = r; bad.magic = 0U;
  CHECK(!app_id_record_valid(&bad), "a wrong magic is refused");
  bad = r; bad.version = 99U;
  CHECK(!app_id_record_valid(&bad), "an unknown version is refused");
  uint8_t erased[sizeof(r)]; memset(erased, 0xFF, sizeof(erased));
  CHECK(!app_id_record_valid((const app_id_record_t *)(void *)erased), "erased flash (all 0xFF) is not a record");
  app_id_record_t notid; app_id_record_pack(&notid, "863110085107293", 1U);
  CHECK(!app_id_record_valid(&notid), "a record holding something that is not a deviceId is refused");
  CHECK(sizeof(app_id_record_t) <= 64U, "small enough for one page");

  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
