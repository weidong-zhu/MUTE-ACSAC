#include "qemu/osdep.h"
#include "hw/block/block.h"
#include "hw/pci/msix.h"
#include "hw/pci/msi.h"
#include "../nvme.h"

#include "ftl.h"


static void fde_xts_aes_encrypt(const void *ctx,
                                 size_t length,
                                 uint8_t *dst,
                                 const uint8_t *src)
{
    const struct AES_fde *aesctx = ctx;

    AES_encrypt(src, dst, &aesctx->enc);
}

static void fde_xts_aes_decrypt(const void *ctx,
                                 size_t length,
                                 uint8_t *dst,
                                 const uint8_t *src)
{
    const struct AES_fde *aesctx = ctx;

    AES_decrypt(src, dst, &aesctx->dec);
}

void fde_encryption(struct ssd *ssd, uint16_t len, const uint8_t *src, uint8_t *dst, uint8_t *iv) {
    xts_encrypt(&(ssd->fde_meta.aesdata), 
                &(ssd->fde_meta.aestweak), 
                fde_xts_aes_encrypt, 
                fde_xts_aes_decrypt, 
                iv, len, dst, src);
}

void fde_decryption(struct ssd *ssd, uint16_t len, const uint8_t *src, uint8_t *dst, uint8_t *iv) {
    xts_decrypt(&(ssd->fde_meta.aesdata), 
                &(ssd->fde_meta.aestweak), 
                fde_xts_aes_encrypt, 
                fde_xts_aes_decrypt, 
                iv, len, dst, src);
}

//void generate_iv(uint64_t lba, uint8_t *dst, uint16_t len) {
    //
//}



/*
 *  blk_num is the number of encryption blocks in flash page
 *  permu is a permutation of set {0,1,...,blk_num-1}
 *  result is the returned rank value which is in a format of binary
 * */
void rank(struct ssd *ssd, uint64_t blk_num, uint64_t *permu, uint8_t *result) {     
}

/*
 *  blk_num is the number of encryption blocks in flash page
 *  rank is the rank value being converted to a permutation
 *  permu is a permutation of set {0,1,...,blk_num-1}
 * */
void unrank(struct ssd *ssd, uint64_t blk_num, uint8_t *rank, uint64_t *permu) {

}
