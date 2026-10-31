/*
 * The Galaxy S9's Bluetooth bring-up, its byte formats and arithmetic (drivers/bluetooth/hakios.bcm_hci.h, what
 * drivers/bluetooth/hakios.bcm4361_bt.c talks to the BCM4361 with) checked on this machine:
 *
 *   cc -O1 -Wall -Wextra tools/bcm_hci_test.c -o bcm_hci_test && ./bcm_hci_test
 * (tools/bcm-hci-test.sh does that under the address and undefined-behaviour sanitizers.)
 *
 * What is checked:
 *   - H4 out: HCI_Reset is 01 03 0C 00, Read Local Version 01 01 10 00, a command with parameters carries them after
 *     its length, and a buffer too small is refused;
 *   - H4 in: a Command Complete comes out whole whether it arrives in one piece or a byte at a time, with junk before
 *     it, after an ACL and an SCO packet (dropped and counted), and two events back to back; an event of length 0 and
 *     one of 255; every byte sequence of a fuzz run ends in a state the parser can go on from, never past its buffer;
 *   - events: Command Complete's opcode, status and return parameters; Command Status; Read Local Version's fields from
 *     the bytes a BCM4361 would send (manufacturer 0x000F) and a reply too short refused; Read Local Name;
 *   - .hcd: made-up patch files (no vendor bytes are in this repository) of Write_RAM records and a Launch_RAM, read as
 *     btbcm_patchram() reads them (btbcm.c:131-158): counted, their name found; a file cut at every length is either
 *     whole or refused as cut, never read past its end; a Launch_RAM in the middle, none at all, a Write_RAM without
 *     its address, a record that is not a vendor command, an empty file: each refused for its own reason;
 *   - the UART's divisor as samsung.c works it out (1088-1110, 1186-1212): 115200 from 200 MHz is UBRDIV 107,
 *     UFRACVAL 8 (115207 baud), and for a range of clocks and rates the result is never further from the rate asked
 *     than the other rounding would be, and UBRDIV and UFRACVAL give back the divisor;
 *   - the expander's bit: only bit 7 of the port changes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/bluetooth/hakios.bcm_hci.h"

static int failures, checks;

#define CHECK(cond, ...) do { checks++; if (!(cond)) { if (failures++ < 40) { printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } } while (0)

static void
h4_out(void)
{
	uint8_t b[300];
	uint8_t p[3] = { 0xaa, 0xbb, 0xcc };

	CHECK(h4_command(b, sizeof(b), HCI_RESET, NULL, 0) == 4, "reset length");
	CHECK(b[0] == 1 && b[1] == 0x03 && b[2] == 0x0c && b[3] == 0, "reset bytes %02x %02x %02x %02x", b[0], b[1], b[2], b[3]);
	CHECK(h4_command(b, sizeof(b), HCI_READ_LOCAL_VERSION, NULL, 0) == 4, "version length");
	CHECK(b[0] == 1 && b[1] == 0x01 && b[2] == 0x10 && b[3] == 0, "version bytes");
	CHECK(h4_command(b, sizeof(b), BCM_WRITE_RAM, p, 3) == 7, "with parameters");
	CHECK(b[1] == 0x4c && b[2] == 0xfc && b[3] == 3 && b[4] == 0xaa && b[6] == 0xcc, "parameters after the length");
	CHECK(h4_command(b, 6, BCM_WRITE_RAM, p, 3) == 0, "too small a buffer");
	CHECK(h4_command(b, 7, BCM_WRITE_RAM, p, 3) == 7, "just big enough");
}

/* feed bytes; count events and copy the last one */
static int
feed(struct h4_rx *rx, const uint8_t *b, size_t n, uint8_t *last, int *junk, int *skipped)
{
	int events = 0;

	for (size_t i = 0; i < n; i++) {
		int r = h4_rx_byte(rx, b[i]);

		if (r == H4_RX_EVENT) {
			events++;
			memcpy(last, rx->event, 2u + rx->event[1]);
		} else if (r == H4_RX_JUNK) {
			(*junk)++;
		} else if (r == H4_RX_SKIPPED) {
			(*skipped)++;
		}
	}
	return events;
}

static void
h4_in(void)
{
	/* Command Complete for Read Local Version: BCM-like values */
	static const uint8_t ver[] = { 0x04, 0x0e, 0x0c, 0x01, 0x01, 0x10, 0x00, 0x06, 0x00, 0x10, 0x06, 0x0f, 0x00, 0x0a, 0x42 };
	static const uint8_t acl[] = { 0x02, 0x01, 0x20, 0x03, 0x00, 0xde, 0xad, 0xbe };
	static const uint8_t sco[] = { 0x03, 0x01, 0x00, 0x02, 0x11, 0x22 };
	static const uint8_t reset_cc[] = { 0x04, 0x0e, 0x04, 0x01, 0x03, 0x0c, 0x00 };
	struct h4_rx rx = { 0 };
	uint8_t last[260], stream[200];
	int junk = 0, skipped = 0, n;
	uint16_t op;
	uint8_t st, rl;
	const uint8_t *ret;
	struct hci_local_version v;

	n = feed(&rx, ver, sizeof(ver), last, &junk, &skipped);
	CHECK(n == 1 && junk == 0, "one event in one piece (%d, junk %d)", n, junk);
	CHECK(hci_command_complete(last, &op, &st, &ret, &rl), "is a Command Complete");
	CHECK(op == HCI_READ_LOCAL_VERSION && st == 0 && rl == 8, "opcode %04x status %u retlen %u", op, st, rl);
	CHECK(hci_local_version(ret, rl, &v), "version parsed");
	CHECK(v.hci_ver == 6 && v.hci_rev == 0x1000 && v.lmp_ver == 6 && v.manufacturer == BCM_MANUFACTURER &&
	    v.lmp_subver == 0x420a, "fields %u %04x %u %04x %04x", v.hci_ver, v.hci_rev, v.lmp_ver, v.manufacturer, v.lmp_subver);
	CHECK(!hci_local_version(ret, 7, &v), "a short reply refused");

	/* junk, ACL, SCO, two events back to back, all a byte at a time */
	size_t k = 0;
	stream[k++] = 0x00;
	stream[k++] = 0xff;
	stream[k++] = 0x01;             /* a command type byte: not something the chip sends */
	memcpy(stream + k, acl, sizeof(acl)); k += sizeof(acl);
	memcpy(stream + k, sco, sizeof(sco)); k += sizeof(sco);
	memcpy(stream + k, reset_cc, sizeof(reset_cc)); k += sizeof(reset_cc);
	memcpy(stream + k, ver, sizeof(ver)); k += sizeof(ver);
	h4_rx_reset(&rx);
	junk = skipped = 0;
	n = feed(&rx, stream, k, last, &junk, &skipped);
	CHECK(n == 2 && junk == 3 && skipped == 2, "events %d junk %d skipped %d", n, junk, skipped);
	CHECK(rx.dropped_bytes == 3 && rx.dropped_packets == 2, "counted %u %u", rx.dropped_bytes, rx.dropped_packets);
	CHECK(last[0] == 0x0e && last[3] == 0x01 && last[4] == 0x10, "the last event is the version's");

	/* the first of the two: HCI_Reset's Command Complete, status 0, no return parameters after the status */
	h4_rx_reset(&rx);
	n = feed(&rx, reset_cc, sizeof(reset_cc), last, &junk, &skipped);
	CHECK(n == 1 && hci_command_complete(last, &op, &st, &ret, &rl) && op == HCI_RESET && st == 0 && rl == 0, "reset's");

	/* lengths 0 and 255 */
	{
		uint8_t e0[] = { 0x04, 0x10, 0x00 }, big[2 + 255 + 1];

		h4_rx_reset(&rx);
		n = feed(&rx, e0, sizeof(e0), last, &junk, &skipped);
		CHECK(n == 1 && last[0] == 0x10 && last[1] == 0, "an event of length 0");
		big[0] = 0x04; big[1] = 0xff; big[2] = 255;
		for (int i = 0; i < 255; i++) {
			big[3 + i] = (uint8_t)i;
		}
		h4_rx_reset(&rx);
		n = feed(&rx, big, sizeof(big), last, &junk, &skipped);
		CHECK(n == 1 && last[1] == 255 && last[2 + 254] == 254, "an event of length 255");
	}

	/* Command Status */
	{
		uint8_t cs[] = { 0x0f, 0x04, 0x00, 0x01, 0x2e, 0xfc };

		CHECK(hci_command_status(cs, &op, &st) && op == BCM_DOWNLOAD_MINIDRIVER && st == 0, "Command Status");
		CHECK(!hci_command_complete(cs, &op, &st, &ret, &rl), "not a Command Complete");
	}

	/* a Command Complete too short to carry an opcode */
	{
		uint8_t bad[] = { 0x0e, 0x02, 0x01, 0x03 };

		CHECK(!hci_command_complete(bad, &op, &st, &ret, &rl), "too short a Command Complete refused");
	}

	/* Read Local Name */
	{
		uint8_t ret_name[248] = { 'B', 'C', 'M', '4', '3', '6', '1', 'B', '2', 0x01, 0 };
		char out[64];

		hci_local_name(ret_name, sizeof(ret_name), out, sizeof(out));
		CHECK(strcmp(out, "BCM4361B2?") == 0, "name %s", out);
		hci_local_name(ret_name, sizeof(ret_name), out, 4);
		CHECK(strcmp(out, "BCM") == 0, "name cut to the buffer: %s", out);
	}

	/* fuzz: random bytes never take the parser past its buffer, and it always comes back to a type byte */
	srand(4361);
	for (int round = 0; round < 2000; round++) {
		uint8_t b[600];
		size_t len = (size_t)(rand() % (int)sizeof(b));

		for (size_t i = 0; i < len; i++) {
			b[i] = (uint8_t)rand();
		}
		h4_rx_reset(&rx);
		feed(&rx, b, len, last, &junk, &skipped);
		CHECK(rx.have <= sizeof(rx.event) || rx.type != H4_EVENT, "past the buffer");
		/* then a good event must still come through once whatever packet was open has ended (an ACL packet may say 65535) */
		for (int i = 0; i < 70000 && rx.type != 0; i++) {
			h4_rx_byte(&rx, 0);
		}
		n = feed(&rx, reset_cc, sizeof(reset_cc), last, &junk, &skipped);
		CHECK(n == 1, "round %d: no event after the noise", round);
	}
}

/* a made-up patch: n Write_RAM records of size each (address, then bytes), a name in the second, then Launch_RAM */
static size_t
make_hcd(uint8_t *out, int n, int size, bool launch)
{
	size_t k = 0;

	for (int r = 0; r < n; r++) {
		uint32_t addr = 0x00200000u + (uint32_t)(r * size);

		out[k++] = 0x4c; out[k++] = 0xfc; out[k++] = (uint8_t)(4 + size);
		out[k++] = (uint8_t)addr; out[k++] = (uint8_t)(addr >> 8); out[k++] = (uint8_t)(addr >> 16); out[k++] = (uint8_t)(addr >> 24);
		for (int i = 0; i < size; i++) {
			out[k + (size_t)i] = (uint8_t)(r * 7 + i);
		}
		if (r == 1 && size >= 40) {
			memcpy(out + k + 5, "BCM4361B2 Test ANT1", 19);
		}
		k += (size_t)size;
	}
	if (launch) {
		out[k++] = 0x4e; out[k++] = 0xfc; out[k++] = 4;
		out[k++] = 0xff; out[k++] = 0xff; out[k++] = 0xff; out[k++] = 0xff;
	}
	return k;
}

static void
hcd(void)
{
	static uint8_t f[64 * 260];
	struct bcm_hcd_info info;
	size_t len = make_hcd(f, 50, 251, true);
	int r;

	r = bcm_hcd_check(f, len, &info);
	CHECK(r == BCM_HCD_OK, "a good file: %s", bcm_hcd_error(r));
	CHECK(info.records == 51 && info.writes == 50 && info.write_bytes == 50 * 251 && info.others == 0, "counted %u %u %u",
	    info.records, info.writes, info.write_bytes);
	CHECK(info.launch_address == 0xffffffffu, "launch address %08x", info.launch_address);
	CHECK(strcmp(info.name, "BCM4361B2 Test ANT1") == 0, "name '%s'", info.name);

	/* records are read as btbcm does: opcode, length, parameters */
	{
		struct bcm_hcd_record rec;
		size_t off = 0;
		int count = 0;

		while (bcm_hcd_next(f, len, &off, &rec) == 1) {
			if (count == 0) {
				CHECK(rec.opcode == BCM_WRITE_RAM && rec.plen == 255 && rec.param[0] == 0x00 && rec.param[2] == 0x20,
				    "first record");
			}
			count++;
		}
		CHECK(count == 51 && off == len, "walked %d to %zu of %zu", count, off, len);
	}

	/* cut at every length: whole or refused, never read past */
	for (size_t cut = 0; cut < len; cut++) {
		uint8_t *copy = malloc(cut ? cut : 1);

		memcpy(copy, f, cut);
		r = bcm_hcd_check(copy, cut, &info);
		CHECK(r != BCM_HCD_OK, "cut at %zu taken as whole", cut);
		CHECK(r == BCM_HCD_EMPTY || r == BCM_HCD_TRUNCATED || r == BCM_HCD_NO_LAUNCH, "cut at %zu: %s", cut, bcm_hcd_error(r));
		free(copy);
	}

	/* no Launch_RAM */
	len = make_hcd(f, 3, 20, false);
	CHECK(bcm_hcd_check(f, len, &info) == BCM_HCD_NO_LAUNCH, "no launch");

	/* Launch_RAM in the middle */
	{
		size_t a = make_hcd(f, 2, 20, true);
		size_t b = make_hcd(f + a, 1, 20, true);

		CHECK(bcm_hcd_check(f, a + b, &info) == BCM_HCD_LAUNCH_EARLY, "an early launch");
	}

	/* a Write_RAM without its address */
	{
		uint8_t s[] = { 0x4c, 0xfc, 0x02, 0x00, 0x00, 0x4e, 0xfc, 0x04, 0xff, 0xff, 0xff, 0xff };

		CHECK(bcm_hcd_check(s, sizeof(s), &info) == BCM_HCD_SHORT_WRITE, "a short write");
	}

	/* a Launch_RAM without its address */
	{
		uint8_t s[] = { 0x4e, 0xfc, 0x00 };

		CHECK(bcm_hcd_check(s, sizeof(s), &info) == BCM_HCD_BAD_LAUNCH, "a bad launch");
	}

	/* not a vendor command: HCI_Reset as the first record */
	{
		uint8_t s[] = { 0x03, 0x0c, 0x00, 0x4e, 0xfc, 0x04, 0xff, 0xff, 0xff, 0xff };

		CHECK(bcm_hcd_check(s, sizeof(s), &info) == BCM_HCD_NOT_VENDOR, "not a vendor command");
	}

	/* another vendor command counted, not refused */
	{
		uint8_t s[] = { 0x18, 0xfc, 0x06, 0, 0, 0, 0, 0, 0, 0x4e, 0xfc, 0x04, 0xff, 0xff, 0xff, 0xff };

		CHECK(bcm_hcd_check(s, sizeof(s), &info) == BCM_HCD_OK && info.others == 1, "another vendor command");
	}

	CHECK(bcm_hcd_check(f, 0, &info) == BCM_HCD_EMPTY, "empty");
	CHECK(strcmp(bcm_hcd_error(BCM_HCD_TRUNCATED), "the file ends inside a record") == 0, "an error's words");
}

static void
divisor(void)
{
	struct exynos_uart_div d;

	CHECK(exynos_uart_divisor(200000000, 115200, &d), "200 MHz, 115200");
	CHECK(d.ubrdiv == 107 && d.fracval == 8 && d.actual == 115207, "UBRDIV %u UFRACVAL %u actual %u", d.ubrdiv, d.fracval,
	    d.actual);
	CHECK(exynos_uart_divisor(200000000, 3000000, &d) && d.ubrdiv == 3 && d.fracval == 3, "3 Mbaud: %u %u", d.ubrdiv, d.fracval);
	CHECK(!exynos_uart_divisor(1000000, 115200, &d), "too slow a clock refused");
	CHECK(!exynos_uart_divisor(0, 115200, &d) && !exynos_uart_divisor(200000000, 0, &d), "zeros refused");

	/* against the rule restated: div = clock / baud, one more if that is nearer, unless the sixteenths are 15 */
	static const uint32_t clocks[] = { 26000000, 100000000, 133000000, 200000000, 400000000 };
	static const uint32_t bauds[] = { 9600, 57600, 115200, 230400, 921600, 1000000, 2000000, 3000000, 4000000 };

	for (unsigned c = 0; c < sizeof(clocks) / sizeof(clocks[0]); c++) {
		for (unsigned b = 0; b < sizeof(bauds) / sizeof(bauds[0]); b++) {
			uint32_t clk = clocks[c], baud = bauds[b], div;

			if (clk / baud < 16) {
				CHECK(!exynos_uart_divisor(clk, baud, &d), "%u from %u refused", baud, clk);
				continue;
			}
			CHECK(exynos_uart_divisor(clk, baud, &d), "%u from %u", baud, clk);
			div = (d.ubrdiv + 1) * 16 + d.fracval;
			CHECK(d.fracval < 16 && clk / div == d.actual, "%u from %u: the registers give the divisor", baud, clk);
			CHECK(div == clk / baud || div == clk / baud + 1, "%u from %u: div %u", baud, clk, div);
			if ((clk / baud & 15) != 15) {
				uint32_t other = div == clk / baud ? div + 1 : div - 1;
				long e = labs((long)baud - (long)(clk / div)), eo = labs((long)baud - (long)(clk / other));

				CHECK(e <= eo, "%u from %u: %ld off, the other way %ld", baud, clk, e, eo);
			}
		}
	}
}

static void
expander(void)
{
	CHECK(pcal_with_bit(0x12, 7, true) == 0x92, "bit 7 on");
	CHECK(pcal_with_bit(0x92, 7, false) == 0x12, "bit 7 off");
	CHECK(pcal_with_bit(0xff, 7, true) == 0xff && pcal_with_bit(0x00, 7, false) == 0x00, "no change when already so");
	for (unsigned v = 0; v < 256; v++) {
		CHECK((pcal_with_bit((uint8_t)v, 7, true) & 0x7f) == (v & 0x7f), "other pins of %02x kept", v);
		CHECK((pcal_with_bit((uint8_t)v, 7, false) & 0x7f) == (v & 0x7f), "other pins of %02x kept", v);
	}
}

int
main(void)
{
	h4_out();
	h4_in();
	hcd();
	divisor();
	expander();
	printf("bcm_hci_test: %d checks, %d failed\n", checks, failures);
	return failures ? 1 : 0;
}
