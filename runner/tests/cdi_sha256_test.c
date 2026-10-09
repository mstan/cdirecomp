#include "cdi_sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int check(const uint8_t *data, size_t size, const char *expected) {
    uint8_t digest[32];char hex[65];
    cdi_sha256(data,size,digest);
    for (unsigned i=0;i<32;i++) sprintf(hex+i*2,"%02x",digest[i]);
    if (!strcmp(hex,expected)) return 0;
    fprintf(stderr,"SHA256 length %zu: %s != %s\n",size,hex,expected);return 1;
}
int main(void) {
    int failures=check(NULL,0,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    failures+=check((const uint8_t *)"abc",3,
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    failures+=check((const uint8_t *)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",56,
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    uint8_t *million=malloc(1000000);
    if (!million) return 1;
    memset(million,'a',1000000);
    failures+=check(million,1000000,
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    free(million);return failures?1:0;
}
