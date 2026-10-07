/* nuubOS: SSH/SCP/SFTP sign-in is root + the device credential only
 * (nuubos-remotectl writes its hash to /etc/shadow). Public-key login is
 * not built, so authorized_keys files have no effect. */
#define DROPBEAR_SVR_PUBKEY_AUTH 0
