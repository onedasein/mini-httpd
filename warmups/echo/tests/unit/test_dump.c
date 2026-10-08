#include "dump.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    const char*   name;
    unsigned char in[8];
    size_t        n;
    size_t        cap;
    size_t        want_need;
    const char*   want_str;
} DumpCase;

static void run_case(const DumpCase* tc) {
    char out[64];
    memset(out, '?', sizeof(out));      /* 哨兵：用来验证 cap=0 时一个字节都没写 */

    size_t need = format_bytes(out, tc->cap, tc->in, tc->n);

    TEST_ASSERT_EQUAL_UINT64_MESSAGE((unsigned long long)tc->want_need,
                                     (unsigned long long)need, tc->name);
    if (tc->cap == 0) {
        TEST_ASSERT_EQUAL_INT_MESSAGE('?', out[0], tc->name);
    } else {
        TEST_ASSERT_EQUAL_STRING_MESSAGE(tc->want_str, out, tc->name);
    }
}

void test_format_bytes_table(void) {
    static const DumpCase cases[] = {
        {"empty",            {0},               0, 64, 0, ""},
        {"plain ascii",      {'h','i'},         2, 64, 2, "hi"},
        {"newline",          {'h','i','\n'},    3, 64, 4, "hi\\n"},
        {"carriage return",  {'\r'},            1, 64, 2, "\\r"},
        {"tab",              {'\t'},            1, 64, 2, "\\t"},
        {"nul byte",         {0x00},            1, 64, 4, "\\x00"},
        {"lowest control",   {0x1f},            1, 64, 4, "\\x1f"},
        {"space",            {' '},             1, 64, 1, " "},
        {"tilde",            {0x7e},            1, 64, 1, "~"},
        {"del",              {0x7f},            1, 64, 4, "\\x7f"},
        {"high byte",        {0xff},            1, 64, 4, "\\xff"},
        {"mixed",            {'h',0x01,0xff},   3, 64, 9, "h\\x01\\xff"},

        /* 截断：返回值仍等于"需要长度"（snprintf 语义），只是写不下那么多 */
        {"cap 0 writes none", {'h','i'},        2,  0, 2, ""},
        {"cap 1 only nul",    {'h','i'},        2,  1, 2, ""},
        {"cap 2 keeps one",   {'h','i'},        2,  2, 2, "h"},
        {"one short",         {'h','i','\n'},   3,  4, 4, "hi\\"},
        {"exact fit",         {'h','i','\n'},   3,  5, 4, "hi\\n"},
        {"mid-escape cut",    {0x01,0x02},      2,  8, 8, "\\x01\\x0"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        run_case(&cases[i]);
    }
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_format_bytes_table);
    return UNITY_END();
}
