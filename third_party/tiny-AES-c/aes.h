#ifndef _AES_H_
#define _AES_H_

#include <stdint.h>
#include <stddef.h>

// #define the macros below to 1/0 to enable/disable the mode of operation.
//
// CBC enables AES encryption in CBC-mode of operation.
// CTR enables encryption in counter-mode.
// ECB enables the basic ECB 16-byte block algorithm. All can be enabled simultaneously.

// The #ifndef-guard allows it to be configured before #include'ing or at compile time.
#ifndef CBC
  #define CBC 1
#endif

#ifndef ECB
  #define ECB 1
#endif

#ifndef CTR
  #define CTR 1
#endif


/* ---- LOCAL PATCH: runtime key length -------------------------------------
 * Stock tiny-AES-c picks AES-128/192/256 at compile time, so one build serves
 * exactly one key size. This project needs both in the same image: the A2687
 * session cipher is AES-128-GCM (key = Z[0:16]) while the C1000's account-key
 * path is AES-256-CBC. Building the file twice under renamed symbols was the
 * alternative and it is worse to read.
 *
 * The patch moves Nk (key words) and Nr (rounds) out of #define and into the
 * context, sizes RoundKey for the largest case, and adds a key length to
 * AES_init_ctx*. Everything else is upstream. The AES-256 CBC vectors in
 * test/host/vectors_data.h cover the added path.
 * -------------------------------------------------------------------------- */

#define AES_BLOCKLEN 16     /* AES is 128-bit block only, whatever the key */
#define AES_KEYLEN_MAX 32
#define AES_keyExpSize_MAX 240

struct AES_ctx
{
  uint8_t RoundKey[AES_keyExpSize_MAX];
  uint8_t Nk;   /* key length in 32-bit words: 4, 6 or 8 */
  uint8_t Nr;   /* rounds: 10, 12 or 14 */
#if (defined(CBC) && (CBC == 1)) || (defined(CTR) && (CTR == 1))
  uint8_t Iv[AES_BLOCKLEN];
#endif
};

/* key_len is 16, 24 or 32; any other value is treated as 16. */
void AES_init_ctx(struct AES_ctx* ctx, const uint8_t* key, size_t key_len);
#if (defined(CBC) && (CBC == 1)) || (defined(CTR) && (CTR == 1))
void AES_init_ctx_iv(struct AES_ctx* ctx, const uint8_t* key, size_t key_len, const uint8_t* iv);
void AES_ctx_set_iv(struct AES_ctx* ctx, const uint8_t* iv);
#endif

#if defined(ECB) && (ECB == 1)
// buffer size is exactly AES_BLOCKLEN bytes; 
// you need only AES_init_ctx as IV is not used in ECB 
// NB: ECB is considered insecure for most uses
void AES_ECB_encrypt(const struct AES_ctx* ctx, uint8_t* buf);
void AES_ECB_decrypt(const struct AES_ctx* ctx, uint8_t* buf);

#endif // #if defined(ECB) && (ECB == !)


#if defined(CBC) && (CBC == 1)
// buffer size MUST be mutile of AES_BLOCKLEN;
// Suggest https://en.wikipedia.org/wiki/Padding_(cryptography)#PKCS7 for padding scheme
// NOTES: you need to set IV in ctx via AES_init_ctx_iv() or AES_ctx_set_iv()
//        no IV should ever be reused with the same key 
void AES_CBC_encrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);
void AES_CBC_decrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);

#endif // #if defined(CBC) && (CBC == 1)


#if defined(CTR) && (CTR == 1)

// Same function for encrypting as for decrypting. 
// IV is incremented for every block, and used after encryption as XOR-compliment for output
// Suggesting https://en.wikipedia.org/wiki/Padding_(cryptography)#PKCS7 for padding scheme
// NOTES: you need to set IV in ctx with AES_init_ctx_iv() or AES_ctx_set_iv()
//        no IV should ever be reused with the same key 
void AES_CTR_xcrypt_buffer(struct AES_ctx* ctx, uint8_t* buf, size_t length);

#endif // #if defined(CTR) && (CTR == 1)


#endif // _AES_H_
