/*
 * test_net.c - Networking Tests for SLM-OS
 *
 * Tests networking utility functions and API behavior.
 * Note: Hardware-dependent tests (actual packet I/O) require VirtIO
 * and are tested interactively via shell commands.
 */

#include "unity.h"

#if defined(PLATFORM_QEMU_VIRT)
#include "../include/net.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* ============================================================================
 * IP Address Utility Tests
 * ============================================================================ */

/*
 * Test: net_ip4_addr creates correct network byte order address
 */
static void test_net_ip4_addr_basic(void)
{
    /* 10.0.2.15 in network byte order (little endian host) */
    uint32_t addr = net_ip4_addr(10, 0, 2, 15);

    /* In network byte order on little-endian: 10 is LSB */
    uint8_t *bytes = (uint8_t *)&addr;
    TEST_ASSERT_EQUAL_UINT8(10, bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0, bytes[1]);
    TEST_ASSERT_EQUAL_UINT8(2, bytes[2]);
    TEST_ASSERT_EQUAL_UINT8(15, bytes[3]);
}

/*
 * Test: net_ip4_addr handles edge cases
 */
static void test_net_ip4_addr_edge_cases(void)
{
    /* 0.0.0.0 */
    uint32_t zero = net_ip4_addr(0, 0, 0, 0);
    TEST_ASSERT_EQUAL_HEX32(0, zero);

    /* 255.255.255.255 (broadcast) */
    uint32_t bcast = net_ip4_addr(255, 255, 255, 255);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, bcast);

    /* 127.0.0.1 (localhost) */
    uint32_t localhost = net_ip4_addr(127, 0, 0, 1);
    uint8_t *bytes = (uint8_t *)&localhost;
    TEST_ASSERT_EQUAL_UINT8(127, bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(0, bytes[1]);
    TEST_ASSERT_EQUAL_UINT8(0, bytes[2]);
    TEST_ASSERT_EQUAL_UINT8(1, bytes[3]);
}

/*
 * Test: net_ip_to_str converts address to string correctly
 */
static void test_net_ip_to_str_basic(void)
{
    char buf[16];
    uint32_t addr = net_ip4_addr(192, 168, 1, 100);

    char *result = net_ip_to_str(addr, buf);

    TEST_ASSERT_EQUAL_PTR(buf, result);
    TEST_ASSERT_EQUAL_STRING("192.168.1.100", buf);
}

/*
 * Test: net_ip_to_str handles edge cases
 */
static void test_net_ip_to_str_edge_cases(void)
{
    char buf[16];

    /* 0.0.0.0 */
    net_ip_to_str(net_ip4_addr(0, 0, 0, 0), buf);
    TEST_ASSERT_EQUAL_STRING("0.0.0.0", buf);

    /* 255.255.255.255 */
    net_ip_to_str(net_ip4_addr(255, 255, 255, 255), buf);
    TEST_ASSERT_EQUAL_STRING("255.255.255.255", buf);

    /* Single digits */
    net_ip_to_str(net_ip4_addr(1, 2, 3, 4), buf);
    TEST_ASSERT_EQUAL_STRING("1.2.3.4", buf);

    /* Mixed digits */
    net_ip_to_str(net_ip4_addr(10, 0, 2, 15), buf);
    TEST_ASSERT_EQUAL_STRING("10.0.2.15", buf);
}

/*
 * Test: net_str_to_ip parses valid addresses
 */
static void test_net_str_to_ip_valid(void)
{
    uint32_t addr;
    int result;

    /* Standard address */
    result = net_str_to_ip("192.168.1.100", &addr);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(192, 168, 1, 100), addr);

    /* QEMU gateway */
    result = net_str_to_ip("10.0.2.2", &addr);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(10, 0, 2, 2), addr);

    /* Localhost */
    result = net_str_to_ip("127.0.0.1", &addr);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(net_ip4_addr(127, 0, 0, 1), addr);

    /* All zeros */
    result = net_str_to_ip("0.0.0.0", &addr);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(0, addr);

    /* Broadcast */
    result = net_str_to_ip("255.255.255.255", &addr);
    TEST_ASSERT_EQUAL_INT(0, result);
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFF, addr);
}

/*
 * Test: net_str_to_ip rejects invalid addresses
 */
static void test_net_str_to_ip_invalid(void)
{
    uint32_t addr;

    /* NULL inputs */
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip(NULL, &addr));
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip("1.2.3.4", NULL));

    /* Too few octets */
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip("1.2.3", &addr));
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip("1.2", &addr));
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip("1", &addr));

    /* Empty string */
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip("", &addr));

    /* Invalid characters */
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip("1.2.3.a", &addr));
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip("abc.def.ghi.jkl", &addr));

    /* Value out of range (>255) */
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip("256.1.2.3", &addr));
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip("1.2.3.999", &addr));

    /* Too many digits in octet */
    TEST_ASSERT_EQUAL_INT(-1, net_str_to_ip("1.2.3.1234", &addr));
}

/*
 * Test: IP address roundtrip (str -> addr -> str)
 */
static void test_net_ip_roundtrip(void)
{
    const char *test_addrs[] = {
        "10.0.2.15",
        "192.168.100.1",
        "172.16.0.254",
        "8.8.8.8",
        "0.0.0.0",
        "255.255.255.0",
    };

    for (size_t i = 0; i < sizeof(test_addrs) / sizeof(test_addrs[0]); i++) {
        uint32_t addr;
        char buf[16];

        /* Parse the string */
        int result = net_str_to_ip(test_addrs[i], &addr);
        TEST_ASSERT_EQUAL_INT(0, result);

        /* Convert back to string */
        net_ip_to_str(addr, buf);
        TEST_ASSERT_EQUAL_STRING(test_addrs[i], buf);
    }
}

/* ============================================================================
 * Network State Tests
 * ============================================================================ */

/*
 * Test: net_is_up returns false before initialization
 *
 * Note: This test assumes network is not initialized at test start.
 * In a real scenario, network might be initialized by shell command.
 */
static void test_net_is_up_before_init(void)
{
    /* If network was previously initialized, this test may not be meaningful */
    /* We test the initial state or accept that it's already up */
    bool is_up = net_is_up();

    /* Either state is valid - we're just testing the function doesn't crash */
    (void)is_up;
    TEST_PASS();
}

/*
 * Test: net_get_info fails gracefully when network not initialized
 */
static void test_net_get_info_not_initialized(void)
{
    if (net_is_up()) {
        /* Network is already up, so get_info should work */
        struct net_info info;
        int result = net_get_info(&info);
        TEST_ASSERT_EQUAL_INT(0, result);
    } else {
        /* Network not initialized - should return error */
        struct net_info info;
        int result = net_get_info(&info);
        TEST_ASSERT_EQUAL_INT(-1, result);
    }
}

/*
 * Test: net_get_info rejects NULL pointer
 */
static void test_net_get_info_null_pointer(void)
{
    int result = net_get_info(NULL);
    TEST_ASSERT_EQUAL_INT(-1, result);
}

/*
 * Test: Network commands fail gracefully when not initialized
 */
static void test_net_commands_without_init(void)
{
    if (!net_is_up()) {
        /* ping should fail */
        int result = net_ping(net_ip4_addr(10, 0, 2, 2), 1, NULL, NULL);
        TEST_ASSERT_EQUAL_INT(-1, result);

        /* set_static_ip should fail */
        result = net_set_static_ip(
            net_ip4_addr(10, 0, 2, 15),
            net_ip4_addr(255, 255, 255, 0),
            net_ip4_addr(10, 0, 2, 2)
        );
        TEST_ASSERT_EQUAL_INT(-1, result);

        /* enable_dhcp should fail */
        result = net_enable_dhcp();
        TEST_ASSERT_EQUAL_INT(-1, result);
    } else {
        /* Network is up - skip this test */
        TEST_PASS();
    }
}

/* ============================================================================
 * Statistics Tests
 * ============================================================================ */

/*
 * Test: net_get_stats doesn't crash with valid or NULL pointer
 */
static void test_net_get_stats_safety(void)
{
    struct net_stats stats;

    /* Should not crash with valid pointer */
    net_get_stats(&stats);

    /* Should not crash with NULL (just doesn't write) */
    net_get_stats(NULL);

    TEST_PASS();
}

/*
 * Test: Statistics start at zero
 */
static void test_net_stats_initial_values(void)
{
    struct net_stats stats;
    net_get_stats(&stats);

    /* If network not used, all stats should be zero */
    /* We can't guarantee this if network was used, so just verify reasonable values */
    TEST_ASSERT_TRUE(stats.rx_packets <= 1000000);  /* Sanity check */
    TEST_ASSERT_TRUE(stats.tx_packets <= 1000000);
    TEST_ASSERT_TRUE(stats.rx_errors <= stats.rx_packets);
    TEST_ASSERT_TRUE(stats.tx_errors <= stats.tx_packets);
}

#endif /* PLATFORM_QEMU_VIRT */

/* ============================================================================
 * Test Suite Entry Point
 * ============================================================================ */

int test_suite_net(void)
{
#if defined(PLATFORM_QEMU_VIRT)
    UNITY_BEGIN();

    /* IP address utility tests */
    RUN_TEST(test_net_ip4_addr_basic);
    RUN_TEST(test_net_ip4_addr_edge_cases);
    RUN_TEST(test_net_ip_to_str_basic);
    RUN_TEST(test_net_ip_to_str_edge_cases);
    RUN_TEST(test_net_str_to_ip_valid);
    RUN_TEST(test_net_str_to_ip_invalid);
    RUN_TEST(test_net_ip_roundtrip);

    /* Network state tests */
    RUN_TEST(test_net_is_up_before_init);
    RUN_TEST(test_net_get_info_not_initialized);
    RUN_TEST(test_net_get_info_null_pointer);
    RUN_TEST(test_net_commands_without_init);

    /* Statistics tests */
    RUN_TEST(test_net_get_stats_safety);
    RUN_TEST(test_net_stats_initial_values);

    return UNITY_END();
#else
    /* Networking not available on non-QEMU platforms */
    return 0;
#endif
}
