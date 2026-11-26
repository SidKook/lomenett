// se050.h -- the SE050 secure element guards the vault key. A 32-byte HMAC
// key (the KEK) lives in the chip with a policy allowing only MAC
// sign/verify/delete (never read out). Each unlock runs the PIN hash through
// HMAC-SHA256 inside the chip, so a flash dump or cloned board is useless.
// APDU encodings from NXP Plug&Trust (se05x_APDU_impl.h / se05x_enums.h).
#pragma once
static bool t1LinkReset();
static int  t1SendApdu(const uint8_t *apdu, int apduLength, uint8_t *response, int responseCapacity);

#define SE_KEK_ID  0x4C4F4D01u                  // "LOM" + 1, user object range
#define SE_KEK_LEN 32

// TLV tags and instruction bytes used below.
#define SE_TAG_POLICY  0x11
#define SE_TAG_1       0x41                     // object id / result value
#define SE_TAG_2       0x42
#define SE_TAG_3       0x43
#define SE_SW_OK       0x9000

static const uint8_t SE_SELECT_APPLET[] = { 0x00,0xA4,0x04,0x00,0x10, 0xA0,0x00,0x00,0x03,0x96,0x54,0x53,0x00,0x00,0x00,0x01,0x03,0x00,0x00,0x00,0x00, 0x00 };

static bool seOpen() {
  uint8_t response[16];
  if (!t1LinkReset()) return false;
  int length = t1SendApdu(SE_SELECT_APPLET, sizeof(SE_SELECT_APPLET), response, sizeof(response));
  return length >= 2 && response[length-2] == 0x90 && response[length-1] == 0x00;
}

// Parse the first TLV (short, 0x81 or 0x82 length). Returns value length or -1.
static int tlvGet(const uint8_t *data, int dataLength, uint8_t tag, const uint8_t **value) {
  if (dataLength < 2 || data[0] != tag) return -1;
  int valueLength, headerLength;
  if (data[1] < 0x80)                          { valueLength = data[1]; headerLength = 2; }
  else if (data[1] == 0x81 && dataLength >= 3) { valueLength = data[2]; headerLength = 3; }
  else if (data[1] == 0x82 && dataLength >= 4) { valueLength = (data[2] << 8) | data[3]; headerLength = 4; }
  else return -1;
  if (headerLength + valueLength > dataLength) return -1;
  *value = data + headerLength;
  return valueLength;
}
static int tlvPut(uint8_t *out, uint8_t tag, const uint8_t *value, int valueLength) {
  out[0] = tag; out[1] = valueLength; memcpy(out + 2, value, valueLength);
  return valueLength + 2;
}
static int tlvPutU32(uint8_t *out, uint8_t tag, uint32_t value) {
  uint8_t bigEndian[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8), (uint8_t)value };
  return tlvPut(out, tag, bigEndian, 4);
}

// Send 80 INS P1 P2 Lc data [Le]. Returns response length (incl. status word) or -1.
static int seCmd(uint8_t ins, uint8_t p1, uint8_t p2, const uint8_t *data, int dataLength, bool expectResponse,
                 uint8_t *response, int responseCapacity, uint16_t *statusWord) {
  uint8_t apdu[80];
  if (dataLength + 6 > 58) return -1;
  apdu[0] = 0x80; apdu[1] = ins; apdu[2] = p1; apdu[3] = p2; apdu[4] = dataLength;
  memcpy(apdu + 5, data, dataLength);
  int apduLength = 5 + dataLength;
  if (expectResponse) apdu[apduLength++] = 0x00;
  int responseLength = t1SendApdu(apdu, apduLength, response, responseCapacity);
  if (responseLength < 2) { *statusWord = 0; return -1; }
  *statusWord = (response[responseLength-2] << 8) | response[responseLength-1];
  return responseLength;
}

static bool seKekExists() {
  if (!seOpen()) return false;
  uint8_t data[8]; int dataLength = tlvPutU32(data, SE_TAG_1, SE_KEK_ID);
  uint8_t response[16]; uint16_t statusWord;
  int responseLength = seCmd(0x04, 0x00, 0x27, data, dataLength, true, response, sizeof(response), &statusWord);   // MGMT / EXIST
  const uint8_t *value;
  int valueLength = responseLength > 2 ? tlvGet(response, responseLength - 2, SE_TAG_1, &value) : -1;
  return statusWord == SE_SW_OK && valueLength == 1 && value[0] == 0x01;
}

static bool seRandom(uint8_t *output, int count) {
  uint8_t data[4] = { SE_TAG_1, 0x02, 0x00, (uint8_t)count };
  uint8_t response[64]; uint16_t statusWord;
  int responseLength = seCmd(0x04, 0x00, 0x49, data, 4, true, response, sizeof(response), &statusWord);         // MGMT / RANDOM
  const uint8_t *value;
  int valueLength = responseLength > 2 ? tlvGet(response, responseLength - 2, SE_TAG_1, &value) : -1;
  if (statusWord != SE_SW_OK || valueLength != count) return false;
  memcpy(output, value, count);
  return true;
}

static bool seKekDelete() {
  if (!seOpen()) return false;
  uint8_t data[8]; int dataLength = tlvPutU32(data, SE_TAG_1, SE_KEK_ID);
  uint8_t response[8]; uint16_t statusWord;
  seCmd(0x04, 0x00, 0x28, data, dataLength, false, response, sizeof(response), &statusWord);                     // MGMT / DELETE_OBJECT
  return statusWord == SE_SW_OK;
}

// Create a fresh KEK (chip TRNG XOR RP ROSC). Policy: SIGN | VERIFY | DELETE only.
static bool seKekProvision() {
  if (!seOpen()) return false;
  if (seKekExists() && !seKekDelete()) return false;
  uint8_t key[SE_KEK_LEN], extraRandom[SE_KEK_LEN];
  if (!seRandom(key, SE_KEK_LEN)) return false;
  roscRandomBytes(extraRandom, SE_KEK_LEN);
  for (int i = 0; i < SE_KEK_LEN; i++) key[i] ^= extraRandom[i];

  uint32_t policyBits = 0x10000000u | 0x08000000u | 0x00040000u;   // ALLOW_SIGN | ALLOW_VERIFY | ALLOW_DELETE
  uint8_t policy[9] = { 8, 0,0,0,0, (uint8_t)(policyBits >> 24), (uint8_t)(policyBits >> 16), (uint8_t)(policyBits >> 8), (uint8_t)policyBits };
  uint8_t data[64]; int dataLength = 0;
  dataLength += tlvPut(data + dataLength, SE_TAG_POLICY, policy, 9);
  dataLength += tlvPutU32(data + dataLength, SE_TAG_1, SE_KEK_ID);
  dataLength += tlvPut(data + dataLength, SE_TAG_3, key, SE_KEK_LEN);
  uint8_t response[8]; uint16_t statusWord;
  seCmd(0x01, 0x05, 0x00, data, dataLength, false, response, sizeof(response), &statusWord);    // WRITE / HMAC key
  memset(key, 0, sizeof(key)); memset(extraRandom, 0, sizeof(extraRandom)); memset(data, 0, sizeof(data));
  Serial.printf("SE050: KEK provision SW %04X\n", statusWord);
  return statusWord == SE_SW_OK;
}

// mac32 = HMAC-SHA256(KEK, input), computed inside the chip.
static bool seKekMac(const uint8_t *input, int inputLength, uint8_t *mac32) {
  if (!seOpen()) return false;
  uint8_t data[64]; int dataLength = 0;
  dataLength += tlvPutU32(data + dataLength, SE_TAG_1, SE_KEK_ID);
  uint8_t macAlgorithm = 0x19;                                     // HMAC_SHA256
  dataLength += tlvPut(data + dataLength, SE_TAG_2, &macAlgorithm, 1);
  dataLength += tlvPut(data + dataLength, SE_TAG_3, input, inputLength);
  uint8_t response[64]; uint16_t statusWord;
  int responseLength = seCmd(0x03, 0x0D, 0x45, data, dataLength, true, response, sizeof(response), &statusWord);   // CRYPTO / MAC one-shot
  const uint8_t *value;
  int valueLength = responseLength > 2 ? tlvGet(response, responseLength - 2, SE_TAG_1, &value) : -1;
  if (statusWord != SE_SW_OK || valueLength != 32) {
    Serial.printf("SE050: MAC SW %04X (%d bytes)\n", statusWord, responseLength);
    return false;
  }
  memcpy(mac32, value, 32);
  memset(response, 0, sizeof(response));
  return true;
}
