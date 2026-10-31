/*
 * The Exynos 9810 TMU's arithmetic and register map (drivers/power/hakios.exynos_tmu.h, what drivers/power/
 * hakios.exynos_tmu.c reads the Galaxy S9's temperatures with) checked on this machine:
 *
 *   cc -O1 -Wall -Wextra tools/exynos_tmu_test.c -o exynos_tmu_test && ./exynos_tmu_test
 * (tools/exynos-tmu-test.sh does that under the sanitizers.)
 *
 * What is checked, each against the BSP (the S9's kernel, Scion-Project/android_kernel_samsung_exynos9810 at e5906667068e;
 * its numbers and formulas are facts restated here, not its code):
 *   - the register offsets: TRIMINFO(0-15) as EXYNOS_TMU_REG_TRIMINFO (exynos_tmu.c:69-73), the current codes of sensors
 *     0-7 as all_temp_show() finds them (1244-1251) and of 8-15 as the CURRENT_TEMP9_8/12_10/15_13 defines (80-82), the
 *     thresholds as exynos9810_tmu_initialize() places them (432-440), the interrupt enables as tmu_irqs_enable() (523-528);
 *   - the zones as the device tree's "sensors" (exynos9810-starlte_eur_open_26.dts 10603, 10629, 10652, 10675: 94, 32, 2,
 *     4) and the trips (10732, 10808, 10942, 11018 where Android starts cooling; 10772, 10856, 10982, 11066 "hot", 115);
 *   - the calibration from a TRIMINFO word: error1 [8:0], error2 [17:9], the type in TRIMINFO0 bit 23, the dt's efuse
 *     value 55 when the fuse is 0 (389-422);
 *   - code to degrees: for every code 0-511 and a grid of fuses, tmu_code_to_bsp_c() equals the BSP's formula
 *     (code_to_temp_with_sensorinfo, 322-341: integer arithmetic, clamped to 10-125, tmu.h:15-16), and the unclamped
 *     millidegrees agree with it to within the BSP's truncation;
 *   - degrees to code (temp_to_code_with_sensorinfo, 257-277) and back: one-point exact; two-point, from 25 C up, never above and below by
 *     at most one code's worth of degrees (60 / (error2 - error1): a degree for a fuse 60 codes apart, as real ones are);
 *   - the worked examples the dt's constants give: with the fuse empty (error1 = 55), 25 C is code 55 and Android's hot
 *     trip, 115 C, is code 145; a two-point sensor with codes 55 at 25 C and 115 at 85 C reads 1 degree a code;
 *   - a fuse whose second point is empty or below the first falls back to one-point arithmetic (the BSP would divide by
 *     zero or a negative span there).
 */
#include <stdio.h>
#include <stdlib.h>

#include "../drivers/power/hakios.exynos_tmu.h"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } } while (0)

/* the BSP's formulas, restated (exynos_tmu.c:322-341, 262-275; first 25, second 85, clamp 10-125) */
static int
bsp_code_to_temp(int two_point, int e1, int e2, int code)
{
	int temp = two_point ? (code - e1) * (85 - 25) / (e2 - e1) + 25 : code - e1 + 25;

	return temp > 125 ? 125 : (temp < 10 ? 10 : temp);
}

static int
bsp_temp_to_code(int two_point, int e1, int e2, int temp)
{
	temp = temp > 125 ? 125 : (temp < 10 ? 10 : temp);
	return two_point ? (temp - 25) * (e2 - e1) / (85 - 25) + e1 : temp + e1 - 25;
}

static void
registers(void)
{
	/* TRIMINFO: ((p) - 0) * 4 for p <= 7, ((p) - 8) * 4 + 0x400 above */
	for (unsigned p = 0; p < 16; p++) {
		CHECK(TMU_TRIMINFO(p) == (p <= 7 ? p * 4 : (p - 8) * 4 + 0x400), "TRIMINFO(%u) = %#x", p, TMU_TRIMINFO(p));
	}
	/* sensors 0-7 by all_temp_show: < 2 offset 0 shift 9 * n, else ((n - 2) / 3 + 1) * 4, shift 9 * ((n - 2) % 3) */
	for (unsigned n = 0; n < 8; n++) {
		unsigned off = n < 2 ? 0 : ((n - 2) / 3 + 1) * 4, shift = n < 2 ? 9 * n : 9 * ((n - 2) % 3);

		CHECK(tmu_temp_reg(n) == 0x40 + off && tmu_temp_shift(n) == shift, "sensor %u: %#x << %u", n, tmu_temp_reg(n),
		    tmu_temp_shift(n));
	}
	/* the defines: CURRENT_TEMP1_0 0x40, 4_2 0x44, 7_5 0x48, 9_8 0x440, 12_10 0x444, 15_13 0x448 */
	static const unsigned expect[16] = { 0x40, 0x40, 0x44, 0x44, 0x44, 0x48, 0x48, 0x48, 0x440, 0x440, 0x444, 0x444, 0x444,
		                             0x448, 0x448, 0x448 };
	for (unsigned n = 0; n < 16; n++) {
		CHECK(tmu_temp_reg(n) == expect[n], "sensor %u in %#x, not %#x", n, tmu_temp_reg(n), expect[n]);
		CHECK(tmu_temp_shift(n) + 9 <= 27, "sensor %u's field fits", n);
	}
	/* thresholds: THD_TEMP0 0x50 for sensor 0, 0x170 + (n - 1) * 0x20 for 1-7, 0x450 + (n - 8) * 0x20 */
	CHECK(tmu_thd_reg(0) == 0x50, "thd 0");
	for (unsigned n = 1; n < 8; n++) {
		CHECK(tmu_thd_reg(n) == 0x170 + (n - 1) * 0x20, "thd %u", n);
	}
	for (unsigned n = 8; n < 16; n++) {
		CHECK(tmu_thd_reg(n) == 0x450 + (n - 8) * 0x20, "thd %u", n);
	}
	/* RISE7 is the first non-passive trip counted from the last: interrupt_count 0, reg_off 0, bit_off (0 + 1) % 2 * 16 */
	CHECK(((0 / 2) * 4) == 0 && (((0 + 1) % 2) * 16) == 16, "RISE7 in [24:16]");
	CHECK(((1 / 2) * 4) == 0 && (((1 + 1) % 2) * 16) == 0, "RISE6 in [8:0]");
	/* interrupt enables: 0x110 + 0x10 n below 5, 0x310 + 0x10 (n - 5) to 7, 0x650 + 0x10 (n - 8) */
	for (unsigned n = 0; n < 16; n++) {
		unsigned e = n < 5 ? 0x110 + 0x10 * n : n < 8 ? 0x310 + 0x10 * (n - 5) : 0x650 + 0x10 * (n - 8);

		CHECK(tmu_inten_reg(n) == e, "inten %u", n);
	}
	CHECK(TMU_CONTROL == 0x20 && TMU_CONTROL1 == 0x24 && TMU_STATUS == 0x28 && TMU_COUNTER_VALUE0 == 0x30 &&
	    TMU_COUNTER_VALUE1 == 0x34 && TMU_AVG_CON == 0x38 && TMU_TRIM0 == 0x3C && TMU_EMUL_CON == 0x160, "the block's registers");
	CHECK(TMU_CTRL_CORE_EN == 1 && TMU_CTRL_TRIP_EN == 0x1000 && TMU_CTRL_TRIP_MODE(0x7u << 13) == 7, "CONTROL's bits");
	CHECK(TMU_CON1_NUM_PROBE(0xfu << 16) == 0xf && TMU_CON1_LPI == 0x400, "CONTROL1's fields");
	CHECK(TMU_SENSE_VALUE == 0x028A, "TEM1051X");
	CHECK(TMU_TOP_PA == 268894208u && TMU_SUB_PA == 268910592u && TMU_SIZE == 1792, "the dt's reg (decimal there)");
}

static void
zones(void)
{
	static const unsigned dt_sensors[4] = { 94, 32, 2, 4 }, dt_block[4] = { 0, 0, 1, 1 };
	static const int cools[4] = { 83, 76, 88, 91 };

	CHECK(TMU_NZONES == 4, "four zones");
	for (unsigned z = 0; z < 4; z++) {
		CHECK(tmu_zones[z].sensors == dt_sensors[z] && tmu_zones[z].block == dt_block[z], "zone %s", tmu_zones[z].name);
		CHECK(tmu_zones[z].android_cools_mc == cools[z] * 1000 && tmu_zones[z].android_hot_mc == 115000, "zone %s trips",
		    tmu_zones[z].name);
	}
	/* BIG and LITTLE share TMU_TOP and no sensor */
	CHECK((tmu_zones[0].sensors & tmu_zones[1].sensors) == 0 && (tmu_zones[2].sensors & tmu_zones[3].sensors) == 0, "disjoint");
}

static void
calibration(void)
{
	struct tmu_cal c;

	/* one point (TRIMINFO0 bit 23 clear): error1 from [8:0], error2 not used */
	c = tmu_cal_of(0x00006c37u, 0);                  /* error1 0x37 = 55, error2 0x36 */
	CHECK(!c.two_point && c.error1 == 55 && c.fell_back == 0, "one point %u", c.error1);
	/* two points: error2 from [17:9] */
	c = tmu_cal_of((115u << 9) | 55u, 1u << 23);
	CHECK(c.two_point && c.error1 == 55 && c.error2 == 115, "two point %u %u", c.error1, c.error2);
	/* other bits of the word do not leak in (the trims at [22:18], bit 23) */
	c = tmu_cal_of(0xffffffffu, 0);
	CHECK(c.error1 == 0x1ff, "mask");
	/* the fuse empty: the dt's 55 */
	c = tmu_cal_of(0, 0);
	CHECK(c.error1 == 55 && c.fell_back == 1, "efuse fallback");
	/* two points with an empty second fuse: 55 >> 9 = 0, so one-point arithmetic */
	c = tmu_cal_of(60, 1u << 23);
	CHECK(!c.two_point && c.fell_back == 2 && c.error1 == 60, "second point empty");
	c = tmu_cal_of((50u << 9) | 60u, 1u << 23);
	CHECK(!c.two_point && c.fell_back == 2, "second point below the first");
}

static void
arithmetic(void)
{
	/* the dt's own numbers: error1 55 (the efuse value), one point: 25 C = 55, 115 C (the hot trip) = 145 */
	struct tmu_cal one = tmu_cal_of(0, 0);

	CHECK(tmu_code_to_mc(&one, 55) == 25000, "55 -> %d", tmu_code_to_mc(&one, 55));
	CHECK(tmu_c_to_code(&one, 115) == 145 && tmu_code_to_mc(&one, 145) == 115000, "hot trip code %d", tmu_c_to_code(&one, 115));
	CHECK(tmu_c_to_code(&one, 110) == 140, "tmu_stop's code");
	/* two points 55 at 25 C, 115 at 85 C: a degree a code */
	struct tmu_cal two = tmu_cal_of((115u << 9) | 55u, 1u << 23);
	CHECK(tmu_code_to_mc(&two, 145) == 115000 && tmu_code_to_mc(&two, 115) == 85000, "two point line");

	/* every code, a grid of fuses: our BSP-equivalent equals the BSP's formula; millidegrees agree with it */
	for (int tp = 0; tp < 2; tp++) {
		for (int e1 = 1; e1 < 200; e1 += 3) {
			for (int span = 20; span < 160; span += 7) {
				int e2 = e1 + span;
				uint32_t trim = ((uint32_t)e2 << 9) | (uint32_t)e1, t0 = tp ? 1u << 23 : 0;
				struct tmu_cal c = tmu_cal_of(trim, t0);

				CHECK(c.two_point == tp && c.error1 == e1 && (!tp || c.error2 == e2), "cal %d %d %d", tp, e1, e2);
				for (int code = 0; code < 512; code++) {
					int bsp = bsp_code_to_temp(tp, e1, e2, code), ours = tmu_code_to_bsp_c(&c, (uint32_t)code);
					int32_t mc = tmu_code_to_mc(&c, (uint32_t)code);
					int whole = (mc - 25000) / 1000 + 25;   /* the BSP truncates (code - e1) * 60 / span toward zero */

					CHECK(bsp == ours, "tp %d e1 %d e2 %d code %d: bsp %d ours %d", tp, e1, e2, code, bsp, ours);
					whole = whole > 125 ? 125 : (whole < 10 ? 10 : whole);
					CHECK(whole == bsp, "tp %d e1 %d e2 %d code %d: %d mC vs %d", tp, e1, e2, code, mc, bsp);
				}
				for (int t = 10; t <= 125; t++) {
					int code = tmu_c_to_code(&c, t), back, step = tp ? (60 + span - 1) / span : 0;

					CHECK(code == bsp_temp_to_code(tp, e1, e2, t), "to code %d %d %d %d", tp, e1, e2, t);
					if (code < 0) {
						continue;               /* below code 0: no sensor gives it */
					}
					/* back to within one code's worth of degrees (60 / span); from 25 up never above (the BSP's
					 * division truncates toward zero, so below 25 the code is rounded the other way) */
					back = tmu_code_to_bsp_c(&c, (uint32_t)code);
					CHECK(back <= t + (t < 25 ? step : 0) && back >= t - step, "round trip %d %d %d %d -> %d -> %d", tp, e1, e2, t, code, back);
				}
			}
		}
	}
	/* what is believed */
	CHECK(!tmu_reading_ok(0, 25000) && !tmu_reading_ok(0x1ff, 25000) && tmu_reading_ok(55, 25000), "codes 0 and 511");
	CHECK(!tmu_reading_ok(300, 300000) && !tmu_reading_ok(5, -50000), "range");
}

int
main(void)
{
	registers();
	zones();
	calibration();
	arithmetic();
	printf("exynos_tmu_test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
