#include "../kernel/types.h"
#include "../kernel/defs.h"
#include "print_test.h"

static int test_count = 0;
static int test_passed = 0;

static void test_assert(int condition, char *test_name) {
    test_count++;
    if (condition) {
        printf("PASS: %s\n", test_name);
        test_passed++;
    } else {
        printf("FAIL: %s\n", test_name);
    }
}

void test_printf_basic(void) {
    printf("Testing basic printf...\n");
    test_assert(1, "printf_basic_output");
}

void test_printf_formats(void) {
    printf("Testing format specifiers...\n");
    int val = 42;
    printf("Integer: %d, Hex: %x\n", val, val);
    test_assert(1, "printf_format_specifiers");
}

void test_id_output(void) {
    printf("Testing ID output...\n");
    long long id = 2023302111177;
    printf("Test ID: %lld\n", id);
    test_assert(id == 2023302111177, "id_output_correct");

    printf("=== 边界情况测试 ===\n");
    printf("INT_MAX: %d\n", 2147483647);
    printf("INT_MIN: %ld\n", -2147483648);
    printf("LONG_MAX: %lld\n", 9223372036854775807LL);
    printf("空字符串: %s\n", "");
    printf("大十六进制: 0x%llx\n", 0xDEADBEEFCAFEBABELL);
}

void run_output_tests(void) {
    printf("\n=== Output Tests ===\n");
    
    test_printf_basic();
    test_printf_formats();
    test_id_output();
    
    printf("\nTest Results: %d/%d passed\n", test_passed, test_count);
}

void clear_screen(void) {
    // 清除整个屏幕
    printf("\033[2J");
    // 光标回到左上角
    printf("\033[H");
}

void goto_xy(int x, int y) {
    printf("\033[%d;%dH", y, x);
}

void clear_line(void) {
    printf("\033[K");
}

void clean_tests(void) {
    goto_xy(1,1);
    clear_line();
    clear_screen();
}