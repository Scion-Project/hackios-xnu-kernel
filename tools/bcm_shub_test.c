/*
 * tools/bcm_shub_test.c: drivers/input/hakios.bcm_shub.h on a computer, under the sanitizers (tools/bcm-shub-test.sh).
 * What it checks:
 *   - the CRC-8 and the framing against the three sample frames in Samsung's S9 driver (bbdpl/bcm_gps_regs.c's "get
 *     version" request and its answer, bbd_rpc_lh.c's sensor packet), byte for byte;
 *   - build -> escape -> receive -> parse round trips with every byte value, sizes 0..SHUB_TL_MAX, every flag;
 *   - the receiver against noise, bad escapes and truncation (nothing read past the buffers: the sanitizers);
 *   - RPC TLVs with one- and two-byte ids and lengths;
 *   - the firmware patch given as argv[1] (devices/starlte/firmware/sensorhub/bcm_sensorhub_a1.bin): every record a
 *     write, 452 of them, 0x00800000-0x0086444c, and the patch's own vector table: the reset vector at 0x00800004 is
 *     PatchAddress 0x00800161;
 *   - SSP data frames (a hand-made one with each record kind, and one the phone's hub sent) and the SSP receive buffer.
 */
#include <stdio.h>
#include <stdlib.h>

#include "../drivers/input/hakios.bcm_shub.h"

static unsigned checks, failures;

#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 12345;
static uint32_t
rnd(void)
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

/* feed a framed packet to a receiver; the number of packets that came out (the last kept in *out) */
static int
receive(struct shub_tl_rx *r, const uint8_t *p, size_t n, struct shub_pkt *out, int *parse)
{
	int got = 0;

	for (size_t i = 0; i < n; i++) {
		if (shub_tl_byte(r, p[i]) == SHUB_TL_PACKET) {
			*parse = shub_tl_parse(r->buf, r->len, out);
			got++;
		}
	}
	return got;
}

static void
samples(void)
{
	/* bcm_gps_regs.c: "get version", after the SSI byte; and the answer in its comment */
	static const uint8_t req[] = { 0xB0, 0x00, 0x00, 0x02, 0x80, 0x03, 0x00, 0x00, 0x00, 0x01, 0xB1, 0xB0, 0x01 };
	static const uint8_t rsp[] = { 0xB0, 0x00, 0x00, 0x08, 0x80, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xFF, 0x00, 0x00,
		                       0xFF, 0x43, 0xB0, 0x01 };
	/* bbd_rpc_lh.c's sample1, after its two SSI length bytes */
	static const uint8_t sensor[] = { 0xB0, 0x00, 0x85, 0x26, 0x00, 0x21, 0x24, 0x22, 0x00, 0x02, 0x00, 0x1E, 0x00,
		0x37, 0x0E, 0xD3, 0x00, 0xCB, 0xFF, 0x50, 0x00, 0xDE, 0x00, 0x56, 0x00, 0xC7, 0xFF, 0x02, 0x00,
		0x00, 0x00, 0x37, 0x00, 0x90, 0xFE, 0x18, 0xFF, 0xB7, 0x46, 0x02, 0x00, 0x00, 0x00, 0xB2, 0xB0, 0x01 };
	static const uint8_t ver_payload[] = { 0x00, 0x01 };
	uint8_t out[64];
	struct shub_pkt p = { .seq = 0, .flags = SHUB_F_INTERNAL | SHUB_F_IGNORE_SEQID, .payload = ver_payload, .size = 2 };
	struct shub_tl_rx r;
	struct shub_pkt got;
	int parse = -1;
	size_t n = shub_tl_build(out, sizeof(out), &p), at = 0;
	uint16_t id = 0, len = 0;
	const uint8_t *data;

	CHECK(n == sizeof(req) && memcmp(out, req, n) == 0);

	memset(&r, 0, sizeof(r));
	CHECK(receive(&r, rsp, sizeof(rsp), &got, &parse) == 1 && parse == SHUB_TL_OK);
	CHECK(got.flags == (SHUB_F_EXTENDED | SHUB_F_INTERNAL) && got.size == 8 && got.payload[0] == 1);
	/* and built back the same */
	n = shub_tl_build(out, sizeof(out), &(struct shub_pkt){ .seq = 0, .flags = SHUB_F_INTERNAL, .payload = got.payload,
	                                                       .size = 8 });
	CHECK(n == sizeof(rsp) && memcmp(out, rsp, n) == 0);

	memset(&r, 0, sizeof(r));
	CHECK(receive(&r, sensor, sizeof(sensor), &got, &parse) == 1 && parse == SHUB_TL_OK);
	CHECK(got.seq == 0x85 && got.size == 0x26 && got.flags == 0);
	CHECK(shub_rpc_next(got.payload, got.size, &at, &id, &data, &len) == 1 && id == SHUB_RPC_SENSOR_RSP && len == 0x24);
	CHECK(shub_le16(data) == len - 2);
	CHECK(shub_rpc_next(got.payload, got.size, &at, &id, &data, &len) == 0);
	n = shub_tl_build(out, sizeof(out), &(struct shub_pkt){ .seq = 0x85, .payload = got.payload, .size = got.size });
	CHECK(n == sizeof(sensor) && memcmp(out, sensor, n) == 0);
}

static void
round_trips(void)
{
	static uint8_t payload[SHUB_TL_MAX], framed[2 * SHUB_TL_MAX + 64];
	struct shub_tl_rx r;

	for (unsigned t = 0; t < 4000; t++) {
		struct shub_pkt p = { 0 }, got;
		uint16_t size = (uint16_t)(t < 300 ? t : rnd() % (SHUB_TL_MAX + 1));
		int parse = -1;
		size_t n;

		for (unsigned i = 0; i < size; i++) {
			payload[i] = (uint8_t)(t % 5 == 0 ? (i % 3 == 0 ? 0xB0 : i % 3 == 1 ? 0x11 : 0x13) : rnd());
		}
		p.seq = (uint8_t)rnd();
		p.flags = (uint16_t)(rnd() & 0x033F);   /* the bits a sender sets itself; 6 and 7 come from the size and flags */
		for (int b = 0; b < 16; b++) {
			p.detail[b] = (uint8_t)rnd();
		}
		p.payload = payload;
		p.size = size;
		n = shub_tl_build(framed, sizeof(framed), &p);
		CHECK(n > 0);
		memset(&r, 0, sizeof(r));
		CHECK(receive(&r, framed, n, &got, &parse) == 1);
		CHECK(parse == SHUB_TL_OK);
		if (parse != SHUB_TL_OK) {
			continue;
		}
		CHECK(got.seq == p.seq && got.size == size && memcmp(got.payload, payload, size) == 0);
		CHECK((got.flags & 0x033F) == p.flags);
		for (int b = 0; b < 16; b++) {
			if ((p.flags & (1u << b)) && b != 6 && b != 7) {
				CHECK(got.detail[b] == p.detail[b]);
			}
		}
		/* every escaped byte is one of the three, nothing raw 0x11/0x13 */
		for (size_t i = 2; i + 2 < n; i++) {
			CHECK(framed[i] != 0x11 && framed[i] != 0x13);
			if (framed[i] == SHUB_ESC) {
				CHECK(framed[i + 1] >= SHUB_ESC_ESC && framed[i + 1] <= SHUB_ESC_XOFF);
				i++;
			}
		}
		/* a flipped bit inside: a packet that still parses has the same size and payload (the CRC does not cover the
		 * sequence number, and one flip can turn an escape into a dropped byte the CRC then catches) */
		if (n > 6) {
			size_t at = 2 + rnd() % (n - 4);

			framed[at] ^= (uint8_t)(1u << (rnd() % 8));
			memset(&r, 0, sizeof(r));
			if (receive(&r, framed, n, &got, &parse) == 1 && parse == SHUB_TL_OK) {
				CHECK(got.size == size && memcmp(got.payload, payload, size) == 0);
			}
		}
	}
	/* too big: refused */
	{
		struct shub_pkt p = { .payload = payload, .size = SHUB_TL_MAX + 1 };

		CHECK(shub_tl_build(framed, sizeof(framed), &p) == 0);
		p.size = 100;
		CHECK(shub_tl_build(framed, 50, &p) == 0);
	}
}

static void
noise(void)
{
	static uint8_t junk[200000];
	struct shub_tl_rx r;
	struct shub_pkt got;

	for (size_t i = 0; i < sizeof(junk); i++) {
		junk[i] = (uint8_t)(i % 7 == 0 ? 0xB0 : rnd());
	}
	memset(&r, 0, sizeof(r));
	for (size_t i = 0; i < sizeof(junk); i++) {
		if (shub_tl_byte(&r, junk[i]) == SHUB_TL_PACKET) {
			(void)shub_tl_parse(r.buf, r.len, &got);
		}
		CHECK(r.len <= sizeof(r.buf));
	}
	for (size_t len = 0; len < 40; len++) {
		uint8_t raw[40];

		for (size_t i = 0; i < len; i++) {
			raw[i] = (uint8_t)rnd();
		}
		raw[2 % (len ? len : 1)] |= 0xC0;       /* sizes and flags extended: the details run past the end */
		(void)shub_tl_parse(raw, len, &got);
	}
}

static void
rpcs(void)
{
	uint8_t buf[3000], data[1000];
	size_t n = 0, at = 0;
	uint16_t id, len;
	const uint8_t *d;

	for (unsigned i = 0; i < sizeof(data); i++) {
		data[i] = (uint8_t)i;
	}
	n += shub_rpc_put(buf + n, 0x05, data, 918);
	n += shub_rpc_put(buf + n, 0x7F, data, 0x7F);
	n += shub_rpc_put(buf + n, 0x402, data, 3);
	n += shub_rpc_put(buf + n, 0x00, NULL, 0);
	CHECK(buf[0] == 0x05 && buf[1] == 0x83 && buf[2] == 0x96);
	CHECK(shub_rpc_next(buf, n, &at, &id, &d, &len) == 1 && id == 0x05 && len == 918 && memcmp(d, data, 918) == 0);
	CHECK(shub_rpc_next(buf, n, &at, &id, &d, &len) == 1 && id == 0x7F && len == 0x7F);
	CHECK(shub_rpc_next(buf, n, &at, &id, &d, &len) == 1 && id == 0x402 && len == 3);
	CHECK(shub_rpc_next(buf, n, &at, &id, &d, &len) == 1 && id == 0 && len == 0);
	CHECK(shub_rpc_next(buf, n, &at, &id, &d, &len) == 0);
	for (size_t cut = 0; cut < 921; cut++) {      /* 921: the first RPC whole */
		at = 0;
		CHECK(shub_rpc_next(buf, cut, &at, &id, &d, &len) == (cut == 0 ? 0 : -1));
	}
}

static void
patch(const char *path)
{
	FILE *f = fopen(path, "rb");
	uint8_t *p;
	long size;
	struct shub_patch_info info;
	size_t at = 0;
	uint32_t addr, expect = 0x00800000, vector = 0;
	const uint8_t *data;
	uint16_t n;

	if (f == NULL) {
		fprintf(stderr, "no patch at %s: its checks skipped\n", path);
		return;
	}
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	fseek(f, 0, SEEK_SET);
	p = malloc((size_t)size);
	CHECK(p != NULL && fread(p, 1, (size_t)size, f) == (size_t)size);
	fclose(f);
	CHECK(size == 413864);
	CHECK(shub_patch_check(p, (size_t)size, &info));
	CHECK(info.records == 452 && info.lo == 0x00800000 && info.hi == 0x0086444c);
	while (shub_patch_next(p, (size_t)size, &at, &addr, &data, &n) == 1) {
		CHECK(addr == expect);  /* contiguous, in order */
		if (addr == 0x00800000) {
			vector = shub_le32(data + 4);
		}
		expect = addr + n;
	}
	CHECK(vector == 0x00800161);
	printf("patch: %u writes, %u bytes, %08x-%08x, reset vector %08x\n", info.records, info.bytes, info.lo, info.hi,
	    vector);
	/* truncated anywhere: refused, never read past */
	for (long cut = 1; cut < 4000; cut += 7) {
		struct shub_patch_info i2;
		uint8_t *c = malloc((size_t)cut);

		memcpy(c, p, (size_t)cut);
		(void)shub_patch_check(c, (size_t)cut, &i2);
		free(c);
	}
	p[0] = 0xF0;
	CHECK(!shub_patch_check(p, (size_t)size, &info));
	free(p);
}

static unsigned seen_samples;
static int16_t seen_x;

static void
on_sample(void *ctx, const struct shub_sample *s)
{
	(void)ctx;
	seen_samples++;
	if (s->sensor == SSP_ACCEL) {
		seen_x = (int16_t)shub_le16(s->data);
		CHECK(s->time == 0x0102030405060708ull);
	}
}

static int32_t seen_accel[3], seen_gyro[3], seen_mag[3];

static void
on_seen(void *ctx, const struct shub_sample *s)
{
	(void)ctx;
	seen_samples++;
	for (int i = 0; i < 3; i++) {
		if (s->sensor == SSP_ACCEL) {
			seen_accel[i] = (int16_t)shub_le16(s->data + 2 * i);
		} else if (s->sensor == SSP_MAG) {
			seen_mag[i] = (int16_t)shub_le16(s->data + 2 * i);
		} else if (s->sensor == SSP_GYRO) {
			seen_gyro[i] = (int32_t)shub_le32(s->data + 4 * i);
		}
	}
}

static void
frames(void)
{
	uint8_t f[200];
	size_t n = 0;
	struct shub_frame_counts c = { 0 };
	struct shub_ssp_rx rx = { 0 };
	uint16_t opts;
	const uint8_t *d;
	size_t dn, used;

	f[n++] = SSP_REC_SENSOR_INIT;
	f[n++] = SSP_REC_SAMPLE;
	f[n++] = SSP_ACCEL;
	f[n++] = 0x34; f[n++] = 0x12; f[n++] = 0; f[n++] = 0; f[n++] = 0; f[n++] = 0x10;
	for (int i = 0; i < 8; i++) {
		f[n++] = (uint8_t)(8 - i);
	}
	f[n++] = SSP_REC_DEBUG; f[n++] = 3; f[n++] = 0; f[n++] = 'h'; f[n++] = 'i'; f[n++] = '!';
	f[n++] = SSP_REC_SAMPLE; f[n++] = SSP_LIGHT;
	memset(f + n, 0, 18 + 8);
	n += 18 + 8;
	f[n++] = SSP_REC_META; f[n++] = 1; f[n++] = SSP_ACCEL;
	CHECK(shub_ssp_frame(f, n, &c, NULL, on_sample, NULL) == SSP_FRAME_OK);
	CHECK(c.init_done == 1 && c.samples == 2 && c.debug == 1 && c.meta == 1 && seen_samples == 2 && seen_x == 0x1234);
	for (size_t cut = 0; cut < n; cut++) {
		struct shub_frame_counts c2 = { 0 };

		(void)shub_ssp_frame(f, cut, &c2, NULL, NULL, NULL);
	}
	f[0] = 0x99;
	CHECK(shub_ssp_frame(f, n, &c, NULL, NULL, NULL) == SSP_FRAME_UNKNOWN);

	/* the receive buffer: a data frame in two pieces, then a READ answer */
	{
		uint8_t hdr[4] = { SSP_HUB2AP_WRITE, 0, 5, 0 }, body[5] = { 1, 2, 3, 4, 5 }, rd[5] = { 0, 0, 0, 0, 0x55 };

		shub_ssp_rx_add(&rx, hdr, 4);
		shub_ssp_rx_add(&rx, body, 2);
		CHECK(shub_ssp_rx_peek(&rx, &opts, &d, &dn, &used) == SSP_RX_NONE);
		shub_ssp_rx_add(&rx, body + 2, 3);
		rx.want_read = 1;
		shub_ssp_rx_add(&rx, rd, 5);
		CHECK(shub_ssp_rx_peek(&rx, &opts, &d, &dn, &used) == SSP_RX_DATA && dn == 5 && d[4] == 5);
		shub_ssp_rx_consume(&rx, used);
		CHECK(shub_ssp_rx_peek(&rx, &opts, &d, &dn, &used) == SSP_RX_READ && dn == 1 && d[0] == 0x55);
		shub_ssp_rx_consume(&rx, used);
		CHECK(rx.len == 0);
	}
	{
		uint8_t h[9];

		shub_ssp_header(h, SSP_SENSOR_ADD, 10, SSP_AP2HUB_WRITE, 0x11223344);
		CHECK(h[0] == 0xA1 && h[1] == 10 && h[2] == 0 && h[3] == 1 && h[4] == 0 && h[5] == 0x44 && h[8] == 0x11);
	}
	CHECK(shub_ssi_room(0x0B) == 3 * 256 && shub_ssi_room(0x40) == 0);

	/* a frame the S9's hub sent (phone 2, 3 November): the magnetometer (8 bytes), the accelerometer lying flat, the
	 * gyroscope at rest */
	{
		static const uint8_t seen[56] = {
			0x37, 0x04, 0x58, 0xfa, 0xfd, 0xfc, 0x51, 0xf4, 0x00, 0x00, 0xc1, 0xd8, 0xbd, 0x29, 0x04, 0x00, 0x00, 0x00,
			0x37, 0x00, 0xac, 0x00, 0xfa, 0xff, 0xf3, 0x0f, 0x80, 0xd5, 0xbd, 0x29, 0x04, 0x00, 0x00, 0x00,
			0x37, 0x01, 0xb8, 0x88, 0x00, 0x00, 0x30, 0xfe, 0xfe, 0xff, 0xf8, 0x30, 0xff, 0xff, 0x80, 0xd5, 0xbd, 0x29,
			0x04, 0x00, 0x00, 0x00 };
		struct shub_frame_counts c3 = { 0 };

		seen_samples = 0;
		seen_x = 0;
		CHECK(shub_ssp_frame(seen, sizeof(seen), &c3, NULL, on_seen, NULL) == SSP_FRAME_OK);
		CHECK(c3.samples == 3 && seen_samples == 3);
		CHECK(seen_accel[0] == 172 && seen_accel[1] == -6 && seen_accel[2] == 4083);
		CHECK(seen_gyro[0] == 35000 && seen_gyro[1] == -66000 && seen_gyro[2] == -53000);
		CHECK(seen_mag[0] == -1448 && seen_mag[1] == -771 && seen_mag[2] == -2991);
	}
}

int
main(int argc, char **argv)
{
	samples();
	round_trips();
	noise();
	rpcs();
	patch(argc > 1 ? argv[1] : "devices/starlte/firmware/sensorhub/bcm_sensorhub_a1.bin");
	frames();
	printf("bcm_shub_test: %u checks, %u failed\n", checks, failures);
	return failures != 0;
}
