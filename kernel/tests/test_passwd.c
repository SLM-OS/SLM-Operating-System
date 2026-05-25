/*
 * test_passwd.c — /mnt/files/etc/passwd database + scrypt verify
 *                 regression coverage (#199d / #896).
 *
 * Targets:
 *
 *   - passwd_any_users on empty / missing file → false.
 *   - passwd_adduser + passwd_verify round-trip (exercises the full
 *     scrypt path via the wolf_heap-backed wolfssl XMALLOC).
 *   - passwd_verify rejects unknown user with PASSWD_E_NOT_FOUND.
 *   - passwd_verify rejects wrong password with PASSWD_E_WRONG.
 *   - passwd_adduser refuses duplicate username (PASSWD_E_EXISTS).
 *   - passwd_deluser refuses removal of the last remaining user
 *     (PASSWD_E_LAST_USER).
 *   - passwd_set rejects over-long username — regression for the
 *     stack-overflow defect addressed in the second review pass.
 *   - passwd_set / passwd_adduser reject NULL inputs cleanly.
 *
 * Each test resets the database by removing the file via littlefs
 * directly (the public API has no "wipe" operation by design — we
 * don't want a shell verb that nukes the password DB). Tests skip
 * cleanly if /mnt/files isn't mounted in this build target.
 *
 * Gated on NET_SSHD so the TU only compiles when the daemon (and
 * therefore passwd.c) is built. Placeholder typedef keeps the file
 * non-empty under -Wpedantic when the gate is off.
 */

#if defined(NET_SSHD)

#include "passwd.h"
#include "littlefs_slm.h"
#include "unity.h"
#include "vfs.h"

#include <stdint.h>
#include <string.h>

#define PASSWD_LFS_PATH   "/etc/passwd"

static struct lfs_mount *resolve_passwd_mount(void)
{
    const char *subpath = NULL;
    return (struct lfs_mount *)vfs_get_mount_ctx("/mnt/files", &subpath);
}

static void reset_passwd_file(void)
{
    struct lfs_mount *mnt = resolve_passwd_mount();
    if (!mnt) return;   /* test will skip itself on the assert below */
    (void)littlefs_remove(mnt, PASSWD_LFS_PATH);
}

static void test_any_users_false_when_file_missing(void)
{
    reset_passwd_file();
    if (!resolve_passwd_mount()) {
        TEST_IGNORE_MESSAGE("/mnt/files not mounted on this target");
    }
    TEST_ASSERT_FALSE(passwd_any_users());
}

static void test_adduser_verify_roundtrip(void)
{
    reset_passwd_file();
    if (!resolve_passwd_mount()) {
        TEST_IGNORE_MESSAGE("/mnt/files not mounted on this target");
    }

    /* Add a user — exercises rng_get_bytes for the salt, derive_hash
     * over scrypt at N=2^15, the PHC-formatted line write, and the
     * atomic rename. */
    TEST_ASSERT_EQUAL_INT(PASSWD_OK,
        passwd_adduser("alice", "correct-horse-battery-staple"));

    /* File now has one entry. */
    TEST_ASSERT_TRUE(passwd_any_users());

    /* Right password verifies. */
    TEST_ASSERT_EQUAL_INT(PASSWD_OK,
        passwd_verify("alice", "correct-horse-battery-staple"));
}

static void test_verify_wrong_password_returns_E_WRONG(void)
{
    reset_passwd_file();
    if (!resolve_passwd_mount()) {
        TEST_IGNORE_MESSAGE("/mnt/files not mounted on this target");
    }
    TEST_ASSERT_EQUAL_INT(PASSWD_OK,
        passwd_adduser("bob", "hunter2"));
    TEST_ASSERT_EQUAL_INT(PASSWD_E_WRONG,
        passwd_verify("bob", "wrong-guess"));
}

static void test_verify_unknown_user_returns_E_NOT_FOUND(void)
{
    reset_passwd_file();
    if (!resolve_passwd_mount()) {
        TEST_IGNORE_MESSAGE("/mnt/files not mounted on this target");
    }
    TEST_ASSERT_EQUAL_INT(PASSWD_OK,
        passwd_adduser("carol", "secret"));
    TEST_ASSERT_EQUAL_INT(PASSWD_E_NOT_FOUND,
        passwd_verify("nonexistent", "anything"));
}

static void test_adduser_refuses_duplicate(void)
{
    reset_passwd_file();
    if (!resolve_passwd_mount()) {
        TEST_IGNORE_MESSAGE("/mnt/files not mounted on this target");
    }
    TEST_ASSERT_EQUAL_INT(PASSWD_OK,    passwd_adduser("dave", "first"));
    TEST_ASSERT_EQUAL_INT(PASSWD_E_EXISTS, passwd_adduser("dave", "second"));
}

static void test_deluser_refuses_last_user(void)
{
    reset_passwd_file();
    if (!resolve_passwd_mount()) {
        TEST_IGNORE_MESSAGE("/mnt/files not mounted on this target");
    }
    TEST_ASSERT_EQUAL_INT(PASSWD_OK, passwd_adduser("eve", "onlyone"));
    /* The bootstrap-safety promise: refuse to delete the only
     * remaining user so the daemon can't lock the operator out. */
    TEST_ASSERT_EQUAL_INT(PASSWD_E_LAST_USER, passwd_deluser("eve"));
    /* User is still there. */
    TEST_ASSERT_EQUAL_INT(PASSWD_OK, passwd_verify("eve", "onlyone"));
}

static void test_set_rejects_overlong_username(void)
{
    /* Regression test for the second-pass Critical: passwd_set on a
     * 200-char username would overflow e.username[33] and corrupt
     * kernel stack. Length-check at the top now rejects it cleanly. */
    char long_name[PASSWD_MAX_USERNAME_LEN + 64u + 1u];
    for (size_t i = 0; i < sizeof(long_name) - 1u; i++) {
        long_name[i] = 'A';
    }
    long_name[sizeof(long_name) - 1u] = '\0';

    int rc = passwd_set(long_name, "anything");
    TEST_ASSERT_EQUAL_INT(PASSWD_E_BAD_ARG, rc);
}

static void test_set_rejects_overlong_password(void)
{
    char long_pw[PASSWD_MAX_PASSWORD_LEN + 16u + 1u];
    for (size_t i = 0; i < sizeof(long_pw) - 1u; i++) {
        long_pw[i] = 'B';
    }
    long_pw[sizeof(long_pw) - 1u] = '\0';

    int rc = passwd_set("name", long_pw);
    TEST_ASSERT_EQUAL_INT(PASSWD_E_BAD_ARG, rc);
}

static void test_adduser_rejects_overlong_username(void)
{
    char long_name[PASSWD_MAX_USERNAME_LEN + 64u + 1u];
    for (size_t i = 0; i < sizeof(long_name) - 1u; i++) {
        long_name[i] = 'A';
    }
    long_name[sizeof(long_name) - 1u] = '\0';
    TEST_ASSERT_EQUAL_INT(PASSWD_E_BAD_ARG,
        passwd_adduser(long_name, "anything"));
}

static void test_null_inputs_return_BAD_ARG(void)
{
    TEST_ASSERT_EQUAL_INT(PASSWD_E_BAD_ARG, passwd_verify(NULL, "x"));
    TEST_ASSERT_EQUAL_INT(PASSWD_E_BAD_ARG, passwd_verify("x", NULL));
    TEST_ASSERT_EQUAL_INT(PASSWD_E_BAD_ARG, passwd_adduser(NULL, "x"));
    TEST_ASSERT_EQUAL_INT(PASSWD_E_BAD_ARG, passwd_adduser("x", NULL));
    TEST_ASSERT_EQUAL_INT(PASSWD_E_BAD_ARG, passwd_set(NULL, "x"));
    TEST_ASSERT_EQUAL_INT(PASSWD_E_BAD_ARG, passwd_set("x", NULL));
    TEST_ASSERT_EQUAL_INT(PASSWD_E_BAD_ARG, passwd_deluser(NULL));
}

int test_suite_passwd(void)
{
    UnityBegin("passwd (scrypt + /mnt/files/etc/passwd)");

    /* Length / null-arg checks run first — they don't need a mounted
     * FS so they work on every target. */
    RUN_TEST(test_set_rejects_overlong_username);
    RUN_TEST(test_set_rejects_overlong_password);
    RUN_TEST(test_adduser_rejects_overlong_username);
    RUN_TEST(test_null_inputs_return_BAD_ARG);

    /* The scrypt-path tests require the wolf_heap + littlefs mount. */
    RUN_TEST(test_any_users_false_when_file_missing);
    RUN_TEST(test_adduser_verify_roundtrip);
    RUN_TEST(test_verify_wrong_password_returns_E_WRONG);
    RUN_TEST(test_verify_unknown_user_returns_E_NOT_FOUND);
    RUN_TEST(test_adduser_refuses_duplicate);
    RUN_TEST(test_deluser_refuses_last_user);

    return UnityEnd();
}

#else  /* !NET_SSHD */

typedef int test_passwd_placeholder_t;

#endif /* NET_SSHD */
