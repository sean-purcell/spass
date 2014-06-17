#include <stdio.h>
#include <string.h>
#include <errno.h>

#include <ibcrypt/chacha.h>
#include <ibcrypt/rand.h>
#include <ibcrypt/scrypt.h>
#include <ibcrypt/sha256.h>

#include <libibur/endian.h>
#include <libibur/util.h>

#include "database.h"
#include "file_db.h"
#include "spasserr.h"

static const char* const magic = "spass\0\0";

#define RWBLOCK 65536
#define min(a, b) ((a) > (b) ? (b) : (a))

int init_dflt_dbf_v00(dbfile_v00_t* dbf, char* password) {
	dbf->nonce = 0;
	dbf->r = 8;
	dbf->p = 1;
	dbf->logN = 16;
	dbf->db = init_db();
	dbf->modified = 1;
	if(cs_rand(dbf->salt, 32) != 0) {
		return CRYPT_ERR;
	}
	int rc;
	if((rc = create_key_v00(password, strlen(password), dbf)) != SUCCESS) {
		return rc;
	}
	return SUCCESS;
}

static int create_header_v00(dbfile_v00_t* dbf, uint8_t header[V00_HEADSIZE]) {
	/* offset  length  purpose */
	
	/*  0       7      magic: "spass\0\0" */
	memcpy(&header[ 0], magic, 7);
	
	/*  7       1      format version number (0x00 for this version) */
	header[ 7] = 0;
	
	/*  8       4      r (big-endian integer; must satisfy r * p < 2^30) */
	encbe32(dbf->r, &header[ 8]);
	
	/* 12       4      p (big-endian integer; must satisfy r * p < 2^30) */
	encbe32(dbf->p, &header[12]);
	
	/* 16       1      logN (scrypt parameter) */
	header[16] = dbf->logN;
	
	/* 17       7      0x00 x 7 for padding */
	memset(&header[17], 0x00, 7);
	
	/* 24      32      salt for scrypt (the parameter that changes for forced key changes) */
	memcpy(&header[24], dbf->salt, 32);
	
	/* 56       8      ctr iv (starts at 1 increments each write, when it gets to 0 force a key change) */
	encbe64(dbf->nonce, &header[56]);
	
	/* 64       8      big endian integer; length of encrypted blob */
	encbe64(serial_size_db(dbf->db), &header[64]);
	
	/* 72      24      random pad bytes */
	cs_rand(&header[72], 24);
	
	/* 96      32      HMAC-SHA256(0 .. 95) */
	HMAC_SHA256_CTX ctx;
	hmac_sha256_init(&ctx, dbf->mackey, 32);
	hmac_sha256_update(&ctx, header, 96);
	hmac_sha256_final(&ctx, &header[96]);
	
	return 0;
}

int write_db_v00(FILE* out, dbfile_v00_t* dbf)  {
	HMAC_SHA256_CTX hctx;
	CHACHA_CTX* cctx;
	uint64_t dbsize = serial_size_db(dbf->db);
	uint8_t header[V00_HEADSIZE];
	uint8_t hbuf[32];
	
	size_t offset;
	
	uint8_t* serial_db = malloc(dbsize);
	if(serial_db == NULL) {
		return ALLOC_FAIL;
	}
	
	serialize_db(dbf->db, serial_db);
	create_header_v00(dbf, header);
	
	hmac_sha256_init(&hctx, dbf->mackey, 32);
	hmac_sha256_update(&hctx, header, V00_HEADSIZE);
	
	if(fwrite(header, V00_HEADSIZE, 1, out) != 1) {
		goto err0;
	}
	
	cctx = init_chacha(dbf->ctrkey, 32, dbf->nonce);
	if(cctx == NULL) {
		goto err0;
	}
	
	stream_chacha(cctx, serial_db, serial_db, dbsize);
	free_chacha(cctx);
	
	offset = 0;
	while(offset < dbsize) {
		size_t writenum = min(RWBLOCK, dbsize-offset);
		if(fwrite(&serial_db[offset], writenum, 1, out) != 1) {
			if(ferror(out)) {
				fprintf(stderr, "write error");
				goto err1;	
			}
		}
		offset += writenum;
	}
	
	hmac_sha256_update(&hctx, serial_db, dbsize);
	hmac_sha256_final(&hctx, hbuf);
	
	if(fwrite(hbuf, 32, 1, out) != 1) {
		goto err1;
	}

	free(serial_db);
	
	return SUCCESS;

err1:
	free(serial_db);
err0:
#ifdef SPASS_FILE_DB_TEST
abort();
#endif
	/* failed! */
	return WRITE_ERR;
}

static int verify_header_v00(uint8_t header[V00_HEADSIZE], dbfile_v00_t* dbf) {
	HMAC_SHA256_CTX ctx;
	uint8_t mac[32];
	
	hmac_sha256_init(&ctx, dbf->mackey, 32);
	hmac_sha256_update(&ctx, header, 96);
	hmac_sha256_final(&ctx, mac);
	
	if(memcmp_ct(&header[96], mac, 32) == 0) {
		return SUCCESS;
	} else {
		return INV_FILE;
	}
}

int read_db_v00(FILE* in, dbfile_v00_t* dbf, char* password, uint32_t plen) {
	int rc = READ_ERR;
	
	uint64_t dbsize;
	uint64_t offset;
	HMAC_SHA256_CTX hctx;
	CHACHA_CTX* cctx;
	uint8_t* serial_db;
	uint8_t header[V00_HEADSIZE];
	uint8_t macfile[32];
	uint8_t maccomp[32];
	if(fread(header, V00_HEADSIZE, 1, in) != 1) {
		goto err0;
	}
	
	/* offset  length  purpose */
	
	/*  0       7      magic: "spass\0\0" */
	if(memcmp(&header[ 0], magic, 7) != 0) {
		rc = INV_FILE;
		goto err0;
	}
	
	/*  7       1      format version number (0x00 for this version) */
	if(header[ 7] != 0x00) {
		rc = INV_FILE;
		goto err0;
	}
	
	/*  8       4      r (big-endian integer; must satisfy r * p < 2^30) */
	dbf->r = decbe32(&header[ 8]);
	
	/* 12       4      p (big-endian integer; must satisfy r * p < 2^30) */
	dbf->p = decbe32(&header[12]);
	
	/* 16       1      logN (scrypt parameter) */
	dbf->logN = header[16];
	
	/* 17       7      0x00 x 7 for padding */
	
	/* 24      32      salt for scrypt (the parameter that changes for forced key changes) */
	memcpy(dbf->salt, &header[24], 32);
	
	/* 56       8      ctr iv (starts at 1 increments each write, when it gets to 0 force a key change) */
	dbf->nonce = decbe64(&header[56]);
	
	/* 64       8      big endian integer; length of encrypted blob */
	dbsize = decbe64(&header[64]);
	
	if(create_key_v00(password, plen, dbf) != SUCCESS) {
		rc = INV_FILE;
		goto err0;
	}
	
	if(verify_header_v00(header, dbf) != SUCCESS) {
		rc = INV_FILE;
		goto err1;
	}
	
	if((serial_db = malloc(dbsize)) == NULL) {
		rc = ALLOC_FAIL;
		goto err1;
	}
	
	hmac_sha256_init(&hctx, dbf->mackey, 32);
	hmac_sha256_update(&hctx, header, V00_HEADSIZE);
	
	offset = 0;
	while(offset < dbsize) {
		size_t readnum = min(RWBLOCK, dbsize-offset);
		if(fread(&serial_db[offset], readnum, 1, in) != 1) {
			goto err2;
		}
		hmac_sha256_update(&hctx, &serial_db[offset], readnum);
		offset += readnum;
	}
	
	if(fread(macfile, 32, 1, in) != 1) {
		goto err2;
	}
	
	hmac_sha256_final(&hctx, maccomp);
	
	if(memcmp_ct(macfile, maccomp, 32) != 0) {
		rc = INV_FILE;
		goto err2;
	}
	
	cctx = init_chacha(dbf->ctrkey, 32, dbf->nonce);
	
	if(cctx == NULL) {
		goto err2;
	}
	
	stream_chacha(cctx, serial_db, serial_db, dbsize);
	free_chacha(cctx);

	cctx = 0;

#ifdef SPASS_FILE_DB_TEST
	printbuf(serial_db, dbsize);
#endif

	dbf->db = deserialize_db(serial_db, dbsize);
	if(dbf->db == NULL) {
		rc = INV_FILE;
		goto err2;
	}

	dbf->modified = 0;

	free(serial_db);
	
	return SUCCESS;

err2:
	free(serial_db);
err1:   /* if we failed we don't want to return the stretched keys */
	memset(dbf->ctrkey, 0, 32);
	memset(dbf->mackey, 0, 32);
	memset(dbf->paskey, 0, 32);
err0:
#ifdef SPASS_FILE_DB_TEST
	abort();
#endif
	/* failed! */
	return rc;
}

int create_key_v00(char* pw, uint32_t pwlen, dbfile_v00_t* dbf) {
	uint8_t dk[96];

	int rc = scrypt(pw, pwlen, dbf->salt, 32, (uint64_t)1 << dbf->logN, dbf->r, dbf->p, 96, dk);
	if(rc != SUCCESS) {
		if(errno == EINVAL) {
			return INV_FILE;
		} else {
			return ALLOC_FAIL;
		}
	}

	memcpy(dbf->ctrkey, &dk[ 0], 32);
	memcpy(dbf->mackey, &dk[32], 32);
	memcpy(dbf->paskey, &dk[64], 32);

	return SUCCESS;
}

int resalt_dbf_v00(dbfile_v00_t* dbf, char* password) {
	dbfile_v00_t cpy;
	AES_KEY okey, nkey;
	int rc;
	uint32_t i;

	/* 256 is passed as constant so this call cannot fail */
	create_key_AES(dbf->paskey, 256, &okey);

	memcpy(&cpy, dbf, sizeof(dbfile_v00_t));

	if(cs_rand(cpy.salt, 32) != 0) {
		rc = CRYPT_ERR;
		goto err;
	}	
	rc = create_key_v00(password, strlen(password), &cpy);
	if(rc != SUCCESS) {
		goto err;
	}

	create_key_AES(cpy.paskey, 256, &nkey);

	cpy.db = init_db();
	if(cpy.db == NULL) {
		rc = ALLOC_FAIL;
		goto err;
	}

	for(i = 0; i < dbf->db->num; i++) {
		passw_t* cpw = dbf->db->pws[i];
		char* pw = dec_pw(cpw, &okey);
		if(pw == NULL) {
			rc = ALLOC_FAIL;
			goto err;
		}

		passw_t* npw = init_pw(cpw->name, pw, cpw->passlen, &nkey);
		rc = db_add_pw(cpy.db, npw);
		if(rc != SUCCESS) {
			goto err;
		}

		memset(pw, 0, cpw->passlen);
		free(pw);
	}

	free_db(dbf->db);
	memcpy(dbf, &cpy, sizeof(dbfile_v00_t));
	memset(&cpy, 0, sizeof(dbfile_v00_t));

	memset(&okey, 0, sizeof(AES_KEY));
	memset(&nkey, 0, sizeof(AES_KEY));

	/* restart the counter */
	dbf->nonce = 1;

	return SUCCESS;

err:
	memset(&okey, 0, sizeof(AES_KEY));
	memset(&nkey, 0, sizeof(AES_KEY));
	if(cpy.db != NULL && cpy.db != dbf->db) {
		free_db(cpy.db);
	}

	memset(&cpy, 0, sizeof(dbfile_v00_t));

	return rc;
}

void clear_dbf_v00(dbfile_v00_t* dbf) {
	if(dbf == 0) {
		return;
	}

	if(dbf->db != 0) {
		free_db(dbf->db);
	}

	memset(dbf, 0, sizeof(dbfile_v00_t));
}
