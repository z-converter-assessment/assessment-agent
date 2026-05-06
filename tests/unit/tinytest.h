/**
 * @file tinytest.h
 * @brief Minimal single-TU C test harness.
 *
 * Usage: include from exactly one .c file, define `void test_*(void)`
 * functions, call RUN_TEST(...) for each, and `return tt_summary();`
 * from main. The state is held in file-scope statics — including this
 * header from multiple TUs that get linked together will produce
 * duplicate-symbol warnings.
 */

#ifndef TINYTEST_H
#define TINYTEST_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tt_pass = 0;
static int tt_fail = 0;
static int tt_current_failed = 0;
static const char *tt_current = "(unknown)";

#define TT_FAIL_(fmt, ...) do {                                               \
	tt_current_failed = 1;                                                    \
	fprintf(stderr, "  FAIL %s:%d %s — " fmt "\n",                             \
	        __FILE__, __LINE__, tt_current, ##__VA_ARGS__);                   \
	return;                                                                   \
} while (0)

#define ASSERT_TRUE(cond) \
	do { if (!(cond)) TT_FAIL_("expected true: %s", #cond); } while (0)

#define ASSERT_FALSE(cond) \
	do { if ((cond))  TT_FAIL_("expected false: %s", #cond); } while (0)

#define ASSERT_EQ(a, b) \
	do { long _ta = (long)(a), _tb = (long)(b);                               \
	     if (_ta != _tb)                                                      \
	         TT_FAIL_("expected %ld, got %ld", _tb, _ta);                     \
	} while (0)

#define ASSERT_STR_EQ(a, b) \
	do { const char *_a = (a), *_b = (b);                                     \
	     if (!_a || !_b || strcmp(_a, _b) != 0)                               \
	         TT_FAIL_("expected \"%s\", got \"%s\"",                           \
	                  _b ? _b : "(null)", _a ? _a : "(null)");                \
	} while (0)

#define ASSERT_NULL(p) \
	do { if ((p) != NULL) TT_FAIL_("expected NULL: %s", #p); } while (0)

#define ASSERT_NOT_NULL(p) \
	do { if ((p) == NULL) TT_FAIL_("expected non-NULL: %s", #p); } while (0)

#define RUN_TEST(fn) do {                                                     \
	tt_current = #fn;                                                         \
	tt_current_failed = 0;                                                    \
	fn();                                                                     \
	if (tt_current_failed) tt_fail++;                                         \
	else                   tt_pass++;                                         \
} while (0)

static int tt_summary(void)
{
	fprintf(stderr, "\n%d passed, %d failed\n", tt_pass, tt_fail);
	return tt_fail == 0 ? 0 : 1;
}

#endif
