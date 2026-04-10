/*
 * AES-128-ECB encryption/decryption for the Astral Pool Viron BLE protocol.
 *
 * The algorithm is a port of pychlorinator/chlorinator.py:
 *   encrypt_mac_key(), encrypt_characteristic(), decrypt_characteristic()
 *
 * mbedTLS is included with the ESP32 Arduino framework – no extra library needed.
 */
#include "crypto.h"
#include <mbedtls/aes.h>
#include <string.h>

// Shared secret hardcoded in all Viron eQuilibrium devices.
static const uint8_t SECRET_KEY[16] = {
    0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
    0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
};

// XOR two byte arrays of potentially different lengths.
// The shorter one is effectively zero-padded on the right.
// result must be at least max(len_a, len_b) bytes.
static void xor_bytes(const uint8_t *a, size_t len_a,
                      const uint8_t *b, size_t len_b,
                      uint8_t *result) {
    size_t len = (len_a > len_b) ? len_a : len_b;
    for (size_t i = 0; i < len; i++) {
        uint8_t va = (i < len_a) ? a[i] : 0;
        uint8_t vb = (i < len_b) ? b[i] : 0;
        result[i]  = va ^ vb;
    }
}

// AES-128-ECB encrypt exactly 16 bytes with SECRET_KEY.
static void aes_encrypt(const uint8_t *in, uint8_t *out) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_enc(&ctx, SECRET_KEY, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

// AES-128-ECB decrypt exactly 16 bytes with SECRET_KEY.
static void aes_decrypt(const uint8_t *in, uint8_t *out) {
    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    mbedtls_aes_setkey_dec(&ctx, SECRET_KEY, 128);
    mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_DECRYPT, in, out);
    mbedtls_aes_free(&ctx);
}

// ─────────────────────────────────────────────────────────────────────────────

void encrypt_mac_key(const uint8_t *session_key,
                     const uint8_t *access_code, size_t code_len,
                     uint8_t *output) {
    // Python: xored = xor_bytes(session_key[16], access_code[4], zero-pad)
    //         return AES_ECB_encrypt(xored)
    uint8_t xored[16];
    xor_bytes(session_key, 16, access_code, code_len, xored);
    aes_encrypt(xored, output);
}

void encrypt_characteristic(const uint8_t *data, const uint8_t *session_key,
                             uint8_t *output) {
    // Python:
    //   xored = xor_bytes(data[20], session_key[16])          → 20 bytes
    //   array = encrypt(xored[:16]) + xored[16:]              → 20 bytes
    //   return array[:4] + encrypt(array[4:])                 → 20 bytes

    uint8_t xored[CHAR_DATA_LEN];
    xor_bytes(data, CHAR_DATA_LEN, session_key, 16, xored);

    uint8_t array[CHAR_DATA_LEN];
    aes_encrypt(xored, array);                 // array[0:16] = encrypt(xored[0:16])
    memcpy(array + 16, xored + 16, 4);         // array[16:20] = xored[16:20]

    uint8_t enc_tail[16];
    aes_encrypt(array + 4, enc_tail);          // encrypt array[4:20]

    memcpy(output,     array,    4);           // output[0:4]  = array[0:4]
    memcpy(output + 4, enc_tail, 16);          // output[4:20] = encrypted
}

void decrypt_characteristic(const uint8_t *data, const uint8_t *session_key,
                             uint8_t *output) {
    // Python:
    //   array = data[:4] + decrypt(data[4:])                  → 20 bytes
    //   array = decrypt(array[:16]) + array[16:]              → 20 bytes
    //   return xor_bytes(array, session_key)

    uint8_t dec_tail[16];
    aes_decrypt(data + 4, dec_tail);           // decrypt data[4:20]

    uint8_t array[CHAR_DATA_LEN];
    memcpy(array,     data,     4);            // array[0:4]  = data[0:4]
    memcpy(array + 4, dec_tail, 16);           // array[4:20] = decrypted

    uint8_t dec_head[16];
    aes_decrypt(array, dec_head);              // decrypt array[0:16]

    uint8_t combined[CHAR_DATA_LEN];
    memcpy(combined,      dec_head,    16);    // combined[0:16] = decrypted
    memcpy(combined + 16, array + 16,   4);    // combined[16:20] = array[16:20]

    xor_bytes(combined, CHAR_DATA_LEN, session_key, 16, output);
}
