#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <libibur/endian.h>

#include <ibcrypt/aes.h>
#include <ibcrypt/rand.h>

#include "password.h"

#ifdef SPASS_PASSWORD_DEBUG
#include <stdio.h>
#endif

passw_t* init_pw(char* name, char* pass, uint32_t plen, AES_KEY* key) {
	passw_t* pw;
	if((pw = (passw_t*) malloc(sizeof(passw_t))) == NULL) {
		errno = ENOMEM;
		goto err0;
	}

	memset(pw, 0, sizeof(passw_t));

	/* name gets terminating byte */
	uint32_t nlen = strlen(name);
	if((pw->name = malloc(nlen + 1)) == NULL) {
		errno = ENOMEM;
		goto err1;
	}

	/* round up to nearest multiple of 16 */
	if((pw->pass = malloc(((plen + 15)/16) * 16)) == NULL) {
		errno = ENOMEM;
		goto err1;
	}
	pw->passlen = plen;

	memcpy(pw->name, name, nlen + 1);
	pw->namelen = nlen;

	cs_rand(pw->iv, 16);

	/* encrypt password in cbc mode */
	memcpy(pw->pass, pass, plen);
	memset(pw->pass+plen, 0x00, ((plen + 15)/16) * 16 - plen);
	encrypt_cbc_AES((uint8_t*) pw->pass, ((plen + 15)/16) * 16, pw->iv, key, pw->pass);

#ifdef SPASS_PASSWORD_DEBUG
	if(pw->name == NULL) {
		fprintf(stderr, "NAME NULL");
		exit(-1);
	}
#endif

	/* success! */
	return pw;

err1:
	free_pw(pw);
err0:
	/* failed! */
	return NULL;
}

passw_t* deserialize_pw(uint8_t* stream) {
	passw_t* pw;
	if((pw = (passw_t*) malloc(sizeof(passw_t))) == NULL) {
		errno = ENOMEM;
		goto err0;
	}

	memset(pw, 0, sizeof(passw_t));

	/* read namelen */
	pw->namelen = decbe32(stream);
	stream += 4;

	/* read passlen */
	pw->passlen = decbe32(stream);
	stream += 4;

	/* allocate buffer for name */
	if((pw->name = (char*) malloc(pw->namelen + 1)) == NULL) {
		errno = ENOMEM;
		goto err1;
	}

	/* allocate buffer for pass */
	if((pw->pass = (uint8_t*) malloc(((pw->passlen+15)/16) * 16)) == NULL) {
		errno = ENOMEM;
		goto err1;
	}

	/* read nonce */
	memcpy(pw->iv, stream, 16);
	stream += 16;

	/* read name */
	memcpy(pw->name, stream, pw->namelen);
	pw->name[pw->namelen] = '\0';
	stream += pw->namelen;

	/* read password */
	memcpy(pw->pass, stream, ((pw->passlen+15)/16) * 16);
	stream += pw->passlen;

	/* success! */
	return pw;

err1:
	free_pw(pw);
err0:
	/* failed! */
	return NULL;
}

void serialize_pw(passw_t* pw, uint8_t* buf) {
	/* name length */
	encbe32(pw->namelen, buf);
	buf += 4;

	/* pass length */
	encbe32(pw->passlen, buf);
	buf += 4;

	/* nonce */
	memcpy(buf, pw->iv, 16);
	buf += 16;

	/* name */
	memcpy(buf, pw->name, pw->namelen);
	buf += pw->namelen;

	/* password (encrypted) */
	memcpy(buf, pw->pass, ((pw->passlen + 15)/16) * 16);
	buf += ((pw->passlen + 15)/16) * 16;

	if(pw->passlen == 0) {
		abort();
	}
}

char* dec_pw(passw_t* pw, AES_KEY* key) {
	char* decpw;
	if((decpw = (char*) malloc(((pw->passlen+15)/16) * 16 + 1)) == NULL) {
		errno = ENOMEM;
		goto err0;
	}
	decrypt_cbc_AES(pw->pass, ((pw->passlen+15)/16) * 16, pw->iv, key, (uint8_t*) decpw);
	decpw[pw->passlen] = '\0';
	return decpw;

err0:
	/* failed! */
	return NULL;
}

void free_pw(passw_t* pw) {
	if(pw->name) {
		/* clear data */
		memset(pw->name, 0, pw->namelen + 1);
		free(pw->name);
	}
	if(pw->pass) {
		/* clear data */
		memset(pw->pass, 0, pw->passlen);
		free(pw->pass);
	}

	/* clear data */
	memset(pw, 0, sizeof(passw_t));
	free(pw);

	/* success! */
}

/* return the size of this password when serialized */
uint32_t serial_size_pw(passw_t* pw) {
	/* sizeof(namelen) + sizeof(passlen) + sizeof(nonce) + len(name) + len(pass) */
	return sizeof(uint32_t) * 2 + sizeof(uint8_t) * 16 + pw->namelen + ((pw->passlen + 15)/16) * 16;
}
