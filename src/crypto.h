#pragma once
#include <stdint.h>
#include <stddef.h>

// All BLE characteristics for this device are 20 bytes on the wire.
#define CHAR_DATA_LEN 20

/**
 * Derive the 16-byte authentication key and store it in `output`.
 *
 * Algorithm (mirrors Python encrypt_mac_key):
 *   xored  = XOR(session_key[16], access_code[N], zero-pad shorter)
 *   output = AES-128-ECB-encrypt(xored, SECRET_KEY)
 *
 * @param session_key  16-byte random key read from UUID_SLAVE_SESSION_KEY
 * @param access_code  ASCII bytes of the 4-character device code (e.g. "L68Z")
 * @param code_len     Length of access_code in bytes
 * @param output       16-byte output buffer (write to UUID_MASTER_AUTHENTICATION)
 */
void encrypt_mac_key(const uint8_t *session_key,
                     const uint8_t *access_code, size_t code_len,
                     uint8_t *output);

/**
 * Encrypt a 20-byte characteristic payload for writing (e.g. action command).
 *
 * @param data        20-byte plaintext payload
 * @param session_key 16-byte session key
 * @param output      20-byte encrypted output
 */
void encrypt_characteristic(const uint8_t *data, const uint8_t *session_key,
                             uint8_t *output);

/**
 * Decrypt a 20-byte characteristic payload read from the device (e.g. state).
 *
 * @param data        20-byte encrypted payload from BLE read
 * @param session_key 16-byte session key
 * @param output      20-byte decrypted output (parse first 11 bytes as state)
 */
void decrypt_characteristic(const uint8_t *data, const uint8_t *session_key,
                             uint8_t *output);
