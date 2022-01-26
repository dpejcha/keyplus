// Copyright 2017 jem@seethis.link
// Licensed under the MIT license (http://opensource.org/licenses/MIT)
#include <string.h>
#include "core/aes.h"
#include "aes/avr-crypto-lib/aes.h"

static aes128_ctx_t aes_ctx;

void aes_key_init(const uint8_t *ekey, const uint8_t *dkey) {
    aes128_init(ekey, &aes_ctx);
}

void aes_encrypt(uint8_t *block) {
    aes128_enc(block, &aes_ctx);
}

void aes_decrypt(uint8_t *block) {
    aes128_dec(block, &aes_ctx);
}
