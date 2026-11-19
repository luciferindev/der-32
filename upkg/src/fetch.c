/*
 * upkg -- transport.
 *
 * Copyright (c) 2026 The DER/32 Project.  All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Supported URL schemes:
 *	file:///path and bare absolute paths	local repositories
 *	https://				remote repositories
 *	http://					only with allow_http = yes
 *
 * On DER/32 the base system's libfetch (with OpenSSL, peer verification on)
 * is used.  Development hosts may build against libcurl instead.  Integrity
 * never depends on the transport: indexes are signature-checked and package
 * archives are checksum-checked against the signed index.
 */

#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(UPKG_WITH_LIBFETCH)
#include <fetch.h>
#elif defined(UPKG_WITH_LIBCURL)
#include <curl/curl.h>
#endif

#include "upkg.h"

#define MAX_DOWNLOAD	(4LL * 1024 * 1024 * 1024)

static int
fetch_local(const char *path, const char *dest)
{
	struct stat st;

	if (stat(path, &st) == -1) {
		upkg_err("%s: %s", path, strerror(errno));
		return -1;
	}
	if (!S_ISREG(st.st_mode)) {
		upkg_err("%s: not a regular file", path);
		return -1;
	}
	return copy_file(path, dest);
}

#if defined(UPKG_WITH_LIBCURL)
static size_t
curl_write(void *p, size_t sz, size_t n, void *arg)
{
	FILE *f = arg;

	if (upkg_interrupted)
		return 0;
	return fwrite(p, sz, n, f);
}

static int
fetch_remote(const struct config *cfg, const char *url, FILE *f)
{
	static int inited;
	CURL *c;
	CURLcode rc;
	long code = 0;

	if (!inited) {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		inited = 1;
	}
	if ((c = curl_easy_init()) == NULL) {
		upkg_err("libcurl initialisation failed");
		return -1;
	}
	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, curl_write);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 5L);
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR,
	    cfg->allow_http ? "https,http" : "https");
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR,
	    cfg->allow_http ? "https,http" : "https");
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 1L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 2L);
	curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)MAX_DOWNLOAD);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 30L);
	curl_easy_setopt(c, CURLOPT_USERAGENT, "upkg/" UPKG_VERSION);
	if (cfg->ca_file)
		curl_easy_setopt(c, CURLOPT_CAINFO, cfg->ca_file);
	rc = curl_easy_perform(c);
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	if (rc != CURLE_OK) {
		upkg_err("%s: %s", url, curl_easy_strerror(rc));
		return -1;
	}
	if (code != 200) {
		upkg_err("%s: HTTP status %ld", url, code);
		return -1;
	}
	return 0;
}
#elif defined(UPKG_WITH_LIBFETCH)
static int
fetch_remote(const struct config *cfg, const char *url, FILE *f)
{
	fetchIO *io;
	char buf[65536];
	ssize_t r;
	long long total = 0;

	if (cfg->ca_file)
		setenv("SSL_CA_CERT_FILE", cfg->ca_file, 1);
	fetchTimeout = 60;
	if ((io = fetchGetURL(url, "")) == NULL) {
		upkg_err("%s: %s", url, fetchLastErrString);
		return -1;
	}
	while ((r = fetchIO_read(io, buf, sizeof(buf))) > 0) {
		if (upkg_interrupted ||
		    fwrite(buf, 1, (size_t)r, f) != (size_t)r) {
			fetchIO_close(io);
			return -1;
		}
		total += r;
		if (total > MAX_DOWNLOAD) {
			upkg_err("%s: download too large", url);
			fetchIO_close(io);
			return -1;
		}
	}
	fetchIO_close(io);
	if (r < 0) {
		upkg_err("%s: %s", url, fetchLastErrString);
		return -1;
	}
	return 0;
}
#else
static int
fetch_remote(const struct config *cfg, const char *url, FILE *f)
{
	(void)cfg;
	(void)f;
	upkg_err("%s: upkg was built without network transport", url);
	return -1;
}
#endif

int
fetch_url(const struct config *cfg, const char *url, const char *dest)
{
	char *tmp;
	FILE *f;
	int fd, rv;

	if (strncmp(url, "file://", 7) == 0)
		return fetch_local(url + 7, dest);
	if (url[0] == '/')
		return fetch_local(url, dest);
	if (strncmp(url, "http://", 7) == 0) {
		if (!cfg->allow_http) {
			upkg_err("%s: plain HTTP is disabled "
			    "(set allow_http = yes in upkg.conf)", url);
			return -1;
		}
	} else if (strncmp(url, "https://", 8) != 0) {
		upkg_err("%s: unsupported URL scheme", url);
		return -1;
	}

	upkg_debug("fetching %s", url);
	tmp = xasprintf("%s.part", dest);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0644);
	if (fd == -1 || (f = fdopen(fd, "w")) == NULL) {
		upkg_err("cannot create %s: %s", tmp, strerror(errno));
		if (fd != -1)
			close(fd);
		free(tmp);
		return -1;
	}
	rv = fetch_remote(cfg, url, f);
	if (fflush(f) != 0 || fsync(fileno(f)) != 0)
		rv = -1;
	fclose(f);
	if (rv == 0 && rename(tmp, dest) == -1) {
		upkg_err("rename %s: %s", tmp, strerror(errno));
		rv = -1;
	}
	if (rv)
		unlink(tmp);
	free(tmp);
	return rv;
}
