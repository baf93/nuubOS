/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * nuubos-web-cert — the per-device TLS certificate of the Web
 * administration (EPIC-038).
 *
 *   nuubos-web-cert KEYFILE CERTFILE NAME
 *
 * Creates a new ECDSA P-256 key and a self-signed certificate for it
 * (10 years, subject CN=NAME, alternative names NAME, NAME.local and
 * nuubos.local), written 0600 through temporary files and renamed, so an
 * interrupted run never leaves a key without its certificate. There is no
 * certificate authority on a LAN console: browsers ask once to trust it.
 * The SHA-256 fingerprint is printed so it can be compared with the one
 * the browser shows.
 */

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int write_pem(const char *path, EVP_PKEY *key, X509 *cert)
{
	char tmp[4096];
	FILE *fp;
	int ok;

	if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
		return 0;
	umask(077);
	fp = fopen(tmp, "w");
	if (!fp)
		return 0;
	ok = key ? PEM_write_PrivateKey(fp, key, NULL, NULL, 0, NULL, NULL) : PEM_write_X509(fp, cert);
	ok = ok && fflush(fp) == 0 && fsync(fileno(fp)) == 0;
	ok = (fclose(fp) == 0) && ok;
	if (!ok || chmod(tmp, 0600) || rename(tmp, path)) {
		unlink(tmp);
		return 0;
	}
	return 1;
}

static int valid_name(const char *s)
{
	size_t n = strlen(s);

	if (!n || n > 63)
		return 0;
	for (; *s; s++)
		if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-'))
			return 0;
	return 1;
}

int main(int argc, char **argv)
{
	EVP_PKEY *key = NULL;
	X509 *cert = NULL;
	X509_EXTENSION *ext;
	X509V3_CTX ctx;
	X509_NAME *name = NULL;
	unsigned char md[EVP_MAX_MD_SIZE];
	unsigned int mdlen = 0;
	unsigned char serial[16];
	BIGNUM *bn = NULL;
	char san[256];
	int rc = 1;

	if (argc != 4 || !valid_name(argv[3])) {
		fprintf(stderr, "Usage: nuubos-web-cert KEYFILE CERTFILE NAME\n");
		return 2;
	}
	key = EVP_EC_gen("P-256");
	cert = X509_new();
	if (!key || !cert)
		goto out;
	if (!X509_set_version(cert, 2))
		goto out;
	/* A random positive serial: browsers remember certificates by issuer
	 * and serial, so a new one must never repeat an old one. */
	{
		FILE *r = fopen("/dev/urandom", "r");

		if (!r || fread(serial, 1, sizeof(serial), r) != sizeof(serial)) {
			if (r)
				fclose(r);
			goto out;
		}
		fclose(r);
	}
	serial[0] &= 0x7f;
	bn = BN_bin2bn(serial, sizeof(serial), NULL);
	if (!bn || !BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert)))
		goto out;
	/* Valid from a day back (the clock may not be synchronized yet). */
	if (!X509_gmtime_adj(X509_getm_notBefore(cert), -86400L) ||
	    !X509_gmtime_adj(X509_getm_notAfter(cert), 10L * 365 * 86400L))
		goto out;
	if (!X509_set_pubkey(cert, key))
		goto out;
	/* OpenSSL 4 hands out the certificate's own name as const: build it
	 * apart and copy it in as subject and issuer. */
	name = X509_NAME_new();
	if (!name ||
	    !X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (const unsigned char *)"nuubOS", -1, -1, 0) ||
	    !X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (const unsigned char *)argv[3], -1, -1, 0) ||
	    !X509_set_subject_name(cert, name) || !X509_set_issuer_name(cert, name))
		goto out;
	snprintf(san, sizeof(san), "DNS:%s,DNS:%s.local,DNS:nuubos.local", argv[3], argv[3]);
	X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);
	ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_subject_alt_name, san);
	if (!ext || !X509_add_ext(cert, ext, -1))
		goto out;
	X509_EXTENSION_free(ext);
	ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_basic_constraints, "critical,CA:FALSE");
	if (!ext || !X509_add_ext(cert, ext, -1))
		goto out;
	X509_EXTENSION_free(ext);
	ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_ext_key_usage, "serverAuth");
	if (!ext || !X509_add_ext(cert, ext, -1))
		goto out;
	X509_EXTENSION_free(ext);
	if (!X509_sign(cert, key, EVP_sha256()))
		goto out;
	if (!write_pem(argv[1], key, NULL) || !write_pem(argv[2], NULL, cert))
		goto out;
	if (X509_digest(cert, EVP_sha256(), md, &mdlen)) {
		for (unsigned int i = 0; i < mdlen; i++)
			printf("%02X%s", md[i], i + 1 < mdlen ? ":" : "\n");
	}
	rc = 0;
out:
	if (rc)
		fprintf(stderr, "nuubos-web-cert: could not create the certificate\n");
	BN_free(bn);
	X509_NAME_free(name);
	X509_free(cert);
	EVP_PKEY_free(key);
	return rc;
}
