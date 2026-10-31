/*
 * The USB gadget's CDC-ACM function (drivers/core/hakios.usb_acm.h, what drivers/core/hakios.dwc3.c answers a host
 * with) checked on this machine: tools/usb-acm-test.sh builds it under the address and undefined-behaviour sanitizers.
 *
 *   - the descriptors: every length, the configuration's total length at both speeds, its interfaces and endpoints,
 *     the CDC functional descriptors (header, call management, ACM, union) naming the right interfaces;
 *   - the requests a host makes when the device is plugged in, in the order Linux (hub.c's two GET_DESCRIPTORs, the
 *     address, the qualifier, the configuration at 9 then full length, the strings, SET_CONFIGURATION, then cdc_acm's
 *     SET_CONTROL_LINE_STATE and SET_LINE_CODING), macOS (GET_STATUS) and Windows (GET_DESCRIPTOR of 64 before the
 *     address) make them, and the answer to each: what is sent, what is expected, the effects for the controller;
 *   - what has to be stalled (a second configuration, BOS, vendor requests, a bad address, a wrong length ...);
 *   - that no IN answer would need a zero-length packet to end it (a full last packet shorter than asked for), which
 *     the controller driver does not send;
 *   - the line console's line assembly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/core/hakios.usb_acm.h"

static int checks, failures;

#define CHECK(cond, ...) do { \
	checks++; \
	if (!(cond)) { \
		failures++; \
		printf("FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

static struct usbacm s;
static struct usbacm_reply r;

static int
setup(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t length)
{
	uint8_t p[8] = { type, req, (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)index, (uint8_t)(index >> 8),
		         (uint8_t)length, (uint8_t)(length >> 8) };
	int a = usbacm_setup(&s, p, &r);

	if (a == USBACM_IN) {
		CHECK(r.len <= length, "IN answer %u longer than wLength %u", r.len, length);
		CHECK(!(r.len < length && r.len % USBACM_EP0_MPS == 0), "IN answer of %u to %u would need a zero-length packet",
		    r.len, length);
	}
	return a;
}

static void
check_config(const uint8_t *d, size_t n, bool hs, uint8_t type)
{
	size_t at = 0;
	int interfaces = 0, endpoints = 0, want_eps = 0, cur_if = -1;
	bool header = false, callmgmt = false, acmfd = false, uni = false;
	uint16_t bulk = hs ? 512 : 64;

	CHECK(n == USBACM_CONFIG_LEN, "configuration length %zu", n);
	CHECK(d[0] == 9 && d[1] == type, "configuration header %u %u", d[0], d[1]);
	CHECK((size_t)(d[2] | d[3] << 8) == n, "wTotalLength %u", d[2] | d[3] << 8);
	CHECK(d[4] == 2 && d[5] == 1, "bNumInterfaces %u bConfigurationValue %u", d[4], d[5]);
	CHECK((d[7] & 0x80) != 0, "bmAttributes bit 7 must be set (%02x)", d[7]);
	while (at < n) {
		uint8_t len = d[at], t = d[at + 1];

		CHECK(len >= 2 && at + len <= n, "descriptor at %zu runs past the end (%u)", at, len);
		if (len < 2 || at + len > n) {
			return;
		}
		if (t == 4) {
			CHECK(len == 9, "interface length %u", len);
			CHECK(endpoints == want_eps, "interface %d had %d endpoints, said %d", cur_if, endpoints, want_eps);
			cur_if = d[at + 2];
			CHECK(cur_if == interfaces, "interface number %d, expected %d", cur_if, interfaces);
			want_eps = d[at + 4];
			endpoints = 0;
			interfaces++;
			if (cur_if == 0) {
				CHECK(d[at + 5] == 2 && d[at + 6] == 2, "interface 0 is not CDC ACM (%u/%u)", d[at + 5], d[at + 6]);
				CHECK(d[at + 8] == 4, "interface 0's string %u", d[at + 8]);
			} else {
				CHECK(d[at + 5] == 0x0a, "interface 1 is not CDC data (%u)", d[at + 5]);
			}
		} else if (t == 5) {
			uint8_t ep = d[at + 2], attr = d[at + 3];
			uint16_t mps = (uint16_t)(d[at + 4] | d[at + 5] << 8);

			CHECK(len == 7, "endpoint length %u", len);
			endpoints++;
			if (cur_if == 0) {
				CHECK(ep == USBACM_EP_NOTIFY && attr == 3 && mps == 16, "notify endpoint %02x %u %u", ep, attr, mps);
				CHECK(hs ? d[at + 6] >= 1 && d[at + 6] <= 16 : d[at + 6] >= 1, "interval %u", d[at + 6]);
			} else {
				CHECK((ep == USBACM_EP_OUT || ep == USBACM_EP_IN) && attr == 2 && mps == bulk, "bulk endpoint %02x %u %u",
				    ep, attr, mps);
			}
		} else if (t == 0x24) {
			CHECK(cur_if == 0, "functional descriptor outside interface 0");
			switch (d[at + 2]) {
			case 0x00:
				header = len == 5 && d[at + 3] == 0x10 && d[at + 4] == 0x01;
				break;
			case 0x01:
				callmgmt = len == 5 && d[at + 4] == 1;
				break;
			case 0x02:
				acmfd = len == 4;
				break;
			case 0x06:
				uni = len == 5 && d[at + 3] == 0 && d[at + 4] == 1;
				break;
			default:
				CHECK(0, "unknown functional descriptor %02x", d[at + 2]);
			}
		} else if (t != type) {
			CHECK(0, "unexpected descriptor type %u at %zu", t, at);
		}
		at += len;
	}
	CHECK(endpoints == want_eps, "last interface had %d endpoints, said %d", endpoints, want_eps);
	CHECK(interfaces == 2, "%d interfaces", interfaces);
	CHECK(header && callmgmt && acmfd && uni, "functional descriptors: header %d call %d acm %d union %d", header, callmgmt,
	    acmfd, uni);
	CHECK(header && (d[9 + 9] == 5), "the header functional descriptor must come first after interface 0");
}

static void
check_string(uint8_t index, const char *want)
{
	char got[128];
	size_t n;

	CHECK(setup(0x80, 6, (uint16_t)(0x0300 | index), 0x0409, 255) == USBACM_IN, "string %u not answered", index);
	CHECK(r.len >= 2 && r.data[0] == r.len && r.data[1] == 3, "string %u header %u/%u", index, r.data[0], r.data[1]);
	n = (r.len - 2) / 2;
	for (size_t i = 0; i < n && i < sizeof(got) - 1; i++) {
		got[i] = (char)r.data[2 + 2 * i];
		CHECK(r.data[3 + 2 * i] == 0, "string %u: not ASCII UTF-16", index);
	}
	got[n < sizeof(got) - 1 ? n : sizeof(got) - 1] = 0;
	CHECK(strcmp(got, want) == 0, "string %u is \"%s\", expected \"%s\"", index, got, want);
}

static void
enumerate(bool hs)
{
	uint8_t cfg[USBACM_CONFIG_LEN];

	usbacm_init(&s);
	s.high_speed = hs;

	/* Windows and Linux: the device descriptor's first 64 (or 8) bytes at address 0 */
	CHECK(setup(0x80, 6, 0x0100, 0, 64) == USBACM_IN && r.len == 18, "GET_DESCRIPTOR(device, 64): %d %u", r.action, r.len);
	CHECK(setup(0x80, 6, 0x0100, 0, 8) == USBACM_IN && r.len == 8 && r.data[7] == 64, "GET_DESCRIPTOR(device, 8)");
	CHECK(setup(0x00, 5, 7, 0, 0) == USBACM_STATUS && (r.effects & USBACM_EFF_ADDRESS) && r.value == 7, "SET_ADDRESS(7)");
	CHECK(s.address == 7, "address %u", s.address);
	CHECK(setup(0x80, 6, 0x0100, 0, 18) == USBACM_IN && r.len == 18, "GET_DESCRIPTOR(device, 18)");
	CHECK(memcmp(r.data, usbacm_device_desc, 18) == 0, "device descriptor");
	CHECK(r.data[0] == 18 && r.data[1] == 1 && r.data[2] == 0x00 && r.data[3] == 0x02 && r.data[4] == 2 && r.data[17] == 1,
	    "device descriptor fields");
	CHECK(r.data[8] == 0x09 && r.data[9] == 0x12 && r.data[10] == 0x01 && r.data[11] == 0x00, "VID/PID 1209:0001");
	CHECK(setup(0x80, 6, 0x0600, 0, 10) == USBACM_IN && r.len == 10 && r.data[1] == 6, "GET_DESCRIPTOR(qualifier)");
	CHECK(setup(0x80, 6, 0x0200, 0, 9) == USBACM_IN && r.len == 9, "GET_DESCRIPTOR(config, 9)");
	CHECK((r.data[2] | r.data[3] << 8) == USBACM_CONFIG_LEN, "wTotalLength in the first 9 bytes");
	CHECK(setup(0x80, 6, 0x0200, 0, 255) == USBACM_IN && r.len == USBACM_CONFIG_LEN, "GET_DESCRIPTOR(config, 255)");
	memcpy(cfg, r.data, r.len);
	check_config(cfg, r.len, hs, 2);
	CHECK(setup(0x80, 6, 0x0700, 0, 255) == USBACM_IN && r.len == USBACM_CONFIG_LEN, "GET_DESCRIPTOR(other speed)");
	memcpy(cfg, r.data, r.len);
	check_config(cfg, r.len, !hs, 7);
	CHECK(setup(0x80, 6, 0x0300, 0, 255) == USBACM_IN && r.len == 4 && r.data[2] == 0x09 && r.data[3] == 0x04, "string 0");
	check_string(1, "HakiOS");
	check_string(2, "HakiOS kernel log and console");
	check_string(3, "0");
	check_string(4, "log");
	CHECK(setup(0x80, 6, 0x0305, 0x0409, 255) == USBACM_STALL, "string 5 must stall");
	CHECK(setup(0x80, 6, 0x0f00, 0, 5) == USBACM_STALL, "BOS must stall (bcdUSB 2.00)");
	CHECK(setup(0x80, 0, 0, 0, 2) == USBACM_IN && r.len == 2 && r.data[0] == 1, "GET_STATUS(device): self-powered");
	CHECK(setup(0x81, 0x0a, 0, 0, 1) == USBACM_STALL, "GET_INTERFACE before a configuration must stall");
	CHECK(setup(0x00, 9, 2, 0, 0) == USBACM_STALL, "SET_CONFIGURATION(2) must stall");
	CHECK(setup(0x00, 9, 1, 0, 0) == USBACM_STATUS && (r.effects & USBACM_EFF_CONFIG) && r.value == 1, "SET_CONFIGURATION(1)");
	CHECK(setup(0x80, 8, 0, 0, 1) == USBACM_IN && r.len == 1 && r.data[0] == 1, "GET_CONFIGURATION");
	CHECK(setup(0x00, 5, 9, 0, 0) == USBACM_STALL, "SET_ADDRESS when configured must stall");
	CHECK(setup(0x81, 0x0a, 0, 1, 1) == USBACM_IN && r.data[0] == 0, "GET_INTERFACE(1)");
	CHECK(setup(0x01, 0x0b, 0, 1, 0) == USBACM_STATUS, "SET_INTERFACE(1, 0)");
	CHECK(setup(0x01, 0x0b, 1, 1, 0) == USBACM_STALL, "SET_INTERFACE(1, 1) must stall");

	/* cdc_acm on open: DTR and RTS, then the line coding */
	CHECK(setup(0x21, 0x22, 3, 0, 0) == USBACM_STATUS && (r.effects & USBACM_EFF_LINES) && s.lines == 3, "SET_CONTROL_LINE_STATE(3)");
	CHECK(setup(0x21, 0x20, 0, 0, 7) == USBACM_OUT && r.len == 7, "SET_LINE_CODING");
	{
		const uint8_t lc[7] = { 0x00, 0x96, 0x00, 0x00, 0, 0, 8 };      /* 38400 8N1 */

		CHECK(usbacm_out_done(&s, lc, 7, &r) == USBACM_STATUS && (r.effects & USBACM_EFF_LINE_CODING), "SET_LINE_CODING data");
		CHECK(setup(0xa1, 0x21, 0, 0, 7) == USBACM_IN && r.len == 7 && memcmp(r.data, lc, 7) == 0, "GET_LINE_CODING");
	}
	CHECK(setup(0x21, 0x20, 0, 0, 7) == USBACM_OUT, "SET_LINE_CODING again");
	CHECK(usbacm_out_done(&s, (const uint8_t *)"\0\0\0", 3, &r) == USBACM_STALL, "a short line coding must stall");
	CHECK(setup(0x21, 0x20, 0, 0, 6) == USBACM_STALL, "SET_LINE_CODING of 6 must stall");
	CHECK(setup(0x21, 0x23, 0xffff, 0, 0) == USBACM_STATUS && (r.effects & USBACM_EFF_BREAK), "SEND_BREAK");
	CHECK(setup(0x21, 0x22, 3, 1, 0) == USBACM_STALL, "a class request to the data interface must stall");
	CHECK(setup(0x40, 0x01, 0, 0, 0) == USBACM_STALL, "a vendor request must stall");
	CHECK(setup(0xc0, 0x01, 0, 0, 4) == USBACM_STALL, "a vendor IN request must stall");

	/* halts */
	CHECK(setup(0x02, 3, 0, USBACM_EP_IN, 0) == USBACM_STATUS && (r.effects & USBACM_EFF_HALT) && r.ep == USBACM_EP_IN, "SET_FEATURE(halt, 0x81)");
	CHECK(setup(0x82, 0, 0, USBACM_EP_IN, 2) == USBACM_IN && r.data[0] == 1, "GET_STATUS(0x81) halted");
	CHECK(setup(0x02, 1, 0, USBACM_EP_IN, 0) == USBACM_STATUS && (r.effects & USBACM_EFF_CLEAR_HALT), "CLEAR_FEATURE(halt, 0x81)");
	CHECK(setup(0x82, 0, 0, USBACM_EP_IN, 2) == USBACM_IN && r.data[0] == 0, "GET_STATUS(0x81) clear");
	CHECK(setup(0x02, 1, 0, 0x03, 0) == USBACM_STALL, "CLEAR_FEATURE(halt) of an endpoint that is not there must stall");
	CHECK(setup(0x02, 1, 0, 0x00, 0) == USBACM_STATUS, "CLEAR_FEATURE(halt, ep0)");

	/* a reset */
	usbacm_reset(&s);
	CHECK(s.address == 0 && s.config == 0 && s.lines == 0, "reset");
	CHECK(setup(0x00, 9, 1, 0, 0) == USBACM_STALL, "SET_CONFIGURATION at address 0 must stall");
	CHECK(setup(0x00, 5, 200, 0, 0) == USBACM_STALL, "SET_ADDRESS(200) must stall");
	CHECK(setup(0x80, 6, 0x0100, 0, 0) == USBACM_STATUS, "GET_DESCRIPTOR with wLength 0 has no data stage");
	CHECK(setup(0x00, 6, 0x0100, 0, 18) == USBACM_STALL, "GET_DESCRIPTOR with the OUT direction must stall");
}

/* every descriptor at every wLength from 1 to 300: never longer than asked, never ending on a full packet short of it */
static void
sweep(void)
{
	static const uint16_t values[] = { 0x0100, 0x0200, 0x0300, 0x0301, 0x0302, 0x0303, 0x0304, 0x0600, 0x0700 };

	for (int hs = 0; hs < 2; hs++) {
		usbacm_init(&s);
		s.high_speed = hs;
		for (size_t v = 0; v < sizeof(values) / sizeof(values[0]); v++) {
			for (uint16_t len = 1; len <= 300; len++) {
				int a = setup(0x80, 6, values[v], (values[v] >> 8) == 3 && (values[v] & 0xff) ? 0x0409 : 0, len);

				CHECK(a == USBACM_IN, "descriptor %04x at %u: %d", values[v], len, a);
			}
		}
	}
}

static void
lines(void)
{
	struct usbacm_line l;
	const char *in = "he\x08\x7f" "help\r\n\r\nsysctl kern.hakios.version\n\x01\x1b[A";
	int got = 0;

	memset(&l, 0, sizeof(l));
	for (const char *c = in; *c != 0; c++) {
		if (usbacm_line_feed(&l, (uint8_t)*c)) {
			got++;
			if (got == 1) {
				CHECK(strcmp(l.line, "help") == 0, "first line \"%s\"", l.line);
			} else if (got == 2) {
				CHECK(strcmp(l.line, "sysctl kern.hakios.version") == 0, "second line \"%s\"", l.line);
			}
		}
	}
	CHECK(got == 2, "%d lines", got);
	CHECK(l.len == 2 && l.line[0] == '[' && l.line[1] == 'A', "control characters dropped: %u", l.len);
	memset(&l, 0, sizeof(l));
	for (int i = 0; i < 500; i++) {
		(void)usbacm_line_feed(&l, 'x');
	}
	CHECK(l.overflow && l.len == sizeof(l.line) - 1, "overflow kept at %u", l.len);
	CHECK(usbacm_line_feed(&l, '\r') && strlen(l.line) == sizeof(l.line) - 1, "an overlong line is cut");
}

int
main(void)
{
	uint8_t buf[8];

	enumerate(true);
	enumerate(false);
	sweep();
	lines();
	CHECK(usbacm_string_desc(buf, sizeof(buf), "abcdefgh") == 8 && buf[0] == 8, "a string cut to its buffer");
	printf("usb_acm_test: %d checks, %d failures\n", checks, failures);
	return failures != 0;
}
