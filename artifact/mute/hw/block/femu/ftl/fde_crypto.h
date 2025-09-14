#ifndef FDECRYPTO_H
#define FDECRYPTO_H


#include "ftl.h"
#include "crypto/init.h"
#include "crypto/xts.h"
#include "crypto/aes.h"

#define KEY_LEN 32

struct AES_fde {
    AES_KEY enc;
    AES_KEY dec;
};

struct crypto_meta {
    uint8_t key1[KEY_LEN];
    uint8_t key2[KEY_LEN];
    struct AES_fde aesdata;
    struct AES_fde aestweak;
};




#endif
