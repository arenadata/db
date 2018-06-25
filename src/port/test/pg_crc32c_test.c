#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include "cmockery.h"
#include "port/pg_crc32c.h"

#include <time.h>


void
test__crc32_correctness(void **state)
{
    const char data[] = {(char)0x6, (char)0x7, (char)0x9, (char)0x3, (char)0x2};
    const unsigned data_length = 5;
    const unsigned long correct_result = 0x84d9dbc2;

    pg_crc32c result = 0;

    INIT_CRC32C(result);
    COMP_CRC32C(result, data, data_length);
    FIN_CRC32C(result);

    assert_true(EQ_CRC32C(result, correct_result));
}

void
test__crc32_speed(void **state)
{
    const char data_f[] = {
            (char)0x0, (char)0x1, (char)0x2, (char)0x3,
            (char)0x4, (char)0x5, (char)0x6, (char)0x7,
            (char)0x8, (char)0x9, (char)0xa, (char)0xb,
            (char)0xc, (char)0xd, (char)0xe, (char)0xf
    };
    const char data_b[] = {
            (char)0xf, (char)0xe, (char)0xd, (char)0xc,
            (char)0xb, (char)0xa, (char)0x9, (char)0x8,
            (char)0x7, (char)0x6, (char)0x5, (char)0x4,
            (char)0x3, (char)0x2, (char)0x1, (char)0x0
    };
    const unsigned data_length = 16;

    pg_crc32c result;
    pg_crc32c base = 0;
    INIT_CRC32C(base);

    struct timeval time_before;
    struct timeval time_after;

    gettimeofday(&(time_before), NULL);
    for (long i = 0; i < 100000; i++) {
        for (long j = 0; j < 10000; j++) {
            result = base;
            COMP_CRC32C(result, data_f, data_length);
            result = base;
            COMP_CRC32C(result, data_b, data_length);
        }
    }
    gettimeofday(&(time_after), NULL);

    double interval_time;
    interval_time = (double)(time_after.tv_sec - time_before.tv_sec) * 1000.0f;
	interval_time += ((double)(time_after.tv_usec - time_before.tv_usec)) / 1000.0f;

    double interval_in_milliseconds = (int)interval_time;
    
    assert_int_equal(interval_time, 0);
}


int
main(int argc, char* argv[])
{
	cmockery_parse_arguments(argc, argv);

	const UnitTest tests[] =
	{
		unit_test(test__crc32_correctness),
        unit_test(test__crc32_speed)
	};

	return run_tests(tests);
}
