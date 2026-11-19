/*
 * upkg -- Ed25519 signing and verification of repository indexes.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * All cryptography is delegated to OpenSSL.  Keys are stored as PEM files:
 * "<name>.key" (PKCS#8 private key, mode 0600) and "<name>.pub" (SPKI public
 * key).  Trusted public keys live in /etc/upkg/keys/.  A signature file holds
 * the 64-byte Ed25519 signature as lowercase hex.
 */

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

#include "upkg.h"

#define ED25519_SIGLEN	64

static void
ssl_err(const char *what)
{
	unsigned long e = ERR_get_error();
	char buf[256];

	if (e) {
		ERR_error_string_n(e, buf, sizeof(buf));
		upkg_err("%s: %s", what, buf);
	} else
		upkg_err("%s failed", what);
}

int
sig_keygen(const char *prefix)
{
	EVP_PKEY_CTX *ctx = NULL;
	EVP_PKEY *key = NULL;
	char *kpath = xasprintf("%s.key", prefix);
	char *ppath = xasprintf("%s.pub", prefix);
	FILE *f = NULL;
	int fd, rv = -1;

	if ((ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, NULL)) == NULL ||
	    EVP_PKEY_keygen_init(ctx) != 1 || EVP_PKEY_keygen(ctx, &key) != 1) {
		ssl_err("key generation");
		goto out;
	}
	fd = open(kpath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
	if (fd == -1 || (f = fdopen(fd, "w")) == NULL) {
		upkg_err("cannot create %s: %s", kpath, strerror(errno));
		if (fd != -1)
			close(fd);
		goto out;
	}
	if (PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL) != 1) {
		ssl_err("writing private key");
		goto out;
	}
	fclose(f);
	f = NULL;
	fd = open(ppath, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
	if (fd == -1 || (f = fdopen(fd, "w")) == NULL) {
		upkg_err("cannot create %s: %s", ppath, strerror(errno));
		if (fd != -1)
			close(fd);
		goto out;
	}
	if (PEM_write_PUBKEY(f, key) != 1) {
		ssl_err("writing public key");
		goto out;
	}
	rv = 0;
	upkg_msg("wrote %s (private, keep secret) and %s (public)", kpath, ppath);
out:
	if (f)
		fclose(f);
	EVP_PKEY_free(key);
	EVP_PKEY_CTX_free(ctx);
	free(kpath);
	free(ppath);
	return rv;
}

int
sig_sign(const char *keyfile, const char *data, size_t len, char **sighex)
{
	EVP_MD_CTX *md = NULL;
	EVP_PKEY *key = NULL;
	unsigned char sig[ED25519_SIGLEN];
	size_t siglen = sizeof(sig);
	FILE *f;
	int rv = -1;

	if ((f = fopen(keyfile, "r")) == NULL) {
		upkg_err("cannot open key %s: %s", keyfile, strerror(errno));
		return -1;
	}
	key = PEM_read_PrivateKey(f, NULL, NULL, NULL);
	fclose(f);
	if (key == NULL || EVP_PKEY_id(key) != EVP_PKEY_ED25519) {
		upkg_err("%s: not an Ed25519 private key", keyfile);
		goto out;
	}
	if ((md = EVP_MD_CTX_new()) == NULL ||
	    EVP_DigestSignInit(md, NULL, NULL, NULL, key) != 1 ||
	    EVP_DigestSign(md, sig, &siglen, (const unsigned char *)data,
	    len) != 1 || siglen != ED25519_SIGLEN) {
		ssl_err("signing");
		goto out;
	}
	*sighex = xmalloc(2 * ED25519_SIGLEN + 1);
	hexencode(sig, ED25519_SIGLEN, *sighex);
	rv = 0;
out:
	EVP_MD_CTX_free(md);
	EVP_PKEY_free(key);
	return rv;
}

static int
verify_one(const char *pubfile, const char *data, size_t len,
    const unsigned char *sig)
{
	EVP_MD_CTX *md = NULL;
	EVP_PKEY *key = NULL;
	FILE *f;
	int ok = 0;

	if ((f = fopen(pubfile, "r")) == NULL)
		return 0;
	key = PEM_read_PUBKEY(f, NULL, NULL, NULL);
	fclose(f);
	if (key == NULL || EVP_PKEY_id(key) != EVP_PKEY_ED25519) {
		upkg_warn("%s: not an Ed25519 public key, ignored", pubfile);
		ERR_clear_error();
		goto out;
	}
	if ((md = EVP_MD_CTX_new()) != NULL &&
	    EVP_DigestVerifyInit(md, NULL, NULL, NULL, key) == 1 &&
	    EVP_DigestVerify(md, sig, ED25519_SIGLEN,
	    (const unsigned char *)data, len) == 1)
		ok = 1;
	ERR_clear_error();
out:
	EVP_MD_CTX_free(md);
	EVP_PKEY_free(key);
	return ok;
}

/*
 * Verify `data' against every trusted key in `keydir' (or only against
 * "<keyname>.pub" when keyname is set).  On success *signer receives the
 * name of the key that verified.
 */
int
sig_verify_dir(const char *keydir, const char *keyname, const char *data,
    size_t len, const char *sighex, char **signer)
{
	unsigned char sig[ED25519_SIGLEN];
	char hex[2 * ED25519_SIGLEN + 1];
	DIR *d;
	struct dirent *de;
	int found = 0;

	/* tolerate surrounding whitespace in the signature file */
	while (*sighex == ' ' || *sighex == '\n' || *sighex == '\t')
		sighex++;
	if (strlen(sighex) < 2 * ED25519_SIGLEN)
		goto bad;
	memcpy(hex, sighex, 2 * ED25519_SIGLEN);
	hex[2 * ED25519_SIGLEN] = '\0';
	for (const char *p = sighex + 2 * ED25519_SIGLEN; *p; p++)
		if (*p != '\n' && *p != ' ' && *p != '\r')
			goto bad;
	if (hexdecode(hex, sig, sizeof(sig)) == -1)
		goto bad;

	if (keyname) {
		char *p = xasprintf("%s/%s.pub", keydir, keyname);
		int ok = verify_one(p, data, len, sig);

		free(p);
		if (ok && signer)
			*signer = xstrdup(keyname);
		return ok ? 0 : -1;
	}
	if ((d = opendir(keydir)) == NULL) {
		upkg_err("no trusted keys: cannot open %s: %s", keydir,
		    strerror(errno));
		return -1;
	}
	while ((de = readdir(d)) != NULL) {
		size_t n = strlen(de->d_name);

		if (n < 5 || strcmp(de->d_name + n - 4, ".pub") != 0)
			continue;
		found = 1;
		char *p = xasprintf("%s/%s", keydir, de->d_name);
		int ok = verify_one(p, data, len, sig);
		free(p);
		if (ok) {
			if (signer)
				*signer = xstrndup(de->d_name, n - 4);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	if (!found)
		upkg_err("no trusted public keys in %s", keydir);
	return -1;
bad:
	upkg_err("malformed signature");
	return -1;
}
