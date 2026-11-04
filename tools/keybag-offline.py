#!/usr/bin/env python3
"""
What someone holding the flash can do with /private/var/hakios-keybag (MiniAppleKeyStore.cpp, docs/keystore.md), and what they cannot,
checked from outside the kernel with independent implementations of Argon2id (argon2-cffi, the reference code) and AES key wrap
(cryptography, OpenSSL's): qemu/run-passcode-test.sh real runs it on the raw volume after boot 11, when the passcode is 999999.

  - the record is found on the raw image, and its digest holds;
  - the kernel's test key is nowhere on the volume in the clear, nor is the passcode;
  - wrong passcodes bring no key: the integrity check of RFC 3394 refuses them;
  - the right passcode brings the roots, and from them the class keys, and a blob the kernel wrapped opens to the test key: the kernel's
    Argon2id, key wrap and derivations are the standard ones, and a guess is all an attacker needs. That is the point of the last line:
    what one guess costs on this machine, and so what a six-digit passcode is worth without a hardware key.

usage: tools/keybag-offline.py IMAGE [--passcode CODE] [--wrong CODE ...] [--no-timing]
needs: pip install argon2-cffi cryptography
"""
import argparse
import hashlib
import hmac
import struct
import sys
import time

try:
	from argon2.low_level import Type, hash_secret_raw
	from cryptography.hazmat.primitives.keywrap import InvalidUnwrap, aes_key_unwrap
except ImportError as e:
	sys.exit("keybag-offline.py: needs argon2-cffi and cryptography (%s)" % e)

MAGIC = 0x4850434b
VERSION = 2
SLOT = 4096
TIERS = ("A", "B", "C", "P")
BAG_AT = 2176                   # offsetof(HKRecord, bag)
BAG_SIZE = 8 + 32 + 8 + 4 * 40 + 32 + 32
KEK_LABEL = b"HakiOS key bag KEK 2"
CLASS_LABEL = b"HakiOS key bag class 2"
TEST_KEY = bytes.fromhex("4b455953544f52452d544553542d4b45592d4d5553542d4e4f542d4c45414b21")
CLASS_AK, CLASS_CK = 6, 7


def parse(block):
	magic, version, generation = struct.unpack_from("<IIQ", block, 0)
	if magic != MAGIC or version != VERSION:
		return None
	digest = block[16:48]
	has_passcode = struct.unpack_from("<I", block, 48)[0]
	blob_length = struct.unpack_from("<I", block, 120)[0]
	if blob_length > 2048:
		return None
	head = block[48:128 + blob_length]
	bag = block[BAG_AT:BAG_AT + BAG_SIZE]
	if hashlib.sha256(head + bag).digest() != digest:
		return None
	mem, passes = struct.unpack_from("<II", bag, 0)
	salt = bag[8:40]
	tiers, migrating = struct.unpack_from("<II", bag, 40)
	wrapped = [bag[48 + 40 * t:48 + 40 * (t + 1)] for t in range(4)]
	return dict(generation=generation, has_passcode=has_passcode, mem=mem, passes=passes, salt=salt, tiers=tiers, migrating=migrating, wrapped=wrapped)


def find_records(image):
	found = []
	for offset in range(0, len(image) - 2 * SLOT, SLOT):
		if image[offset:offset + 4] == struct.pack("<I", MAGIC):
			record = parse(image[offset:offset + SLOT])
			if record:
				record["offset"] = offset
				found.append(record)
	return found


def kek_of(code, record):
	derived = hash_secret_raw(code, record["salt"], time_cost=record["passes"], memory_cost=record["mem"], parallelism=1, hash_len=32, type=Type.ID, version=19)
	return hmac.new(derived, KEK_LABEL, hashlib.sha256).digest()


def roots_of(code, record):
	kek = kek_of(code, record)
	roots = {}
	for t, name in enumerate(TIERS):
		if record["tiers"] & (1 << t):
			try:
				roots[name] = aes_key_unwrap(kek, record["wrapped"][t])
			except InvalidUnwrap:
				return None
	return roots


def class_key(root, number):
	return hmac.new(root, CLASS_LABEL + bytes([number]), hashlib.sha256).digest()


def opens_a_blob(image, key):
	"""A blob the kernel wrapped, wherever on the volume the test's file is: at the start of a 4 KB block."""
	for offset in range(0, len(image), SLOT):
		candidate = image[offset:offset + 40]
		try:
			if aes_key_unwrap(key, candidate) == TEST_KEY:
				return offset
		except (InvalidUnwrap, ValueError):
			continue
	return None


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument("image")
	ap.add_argument("--passcode", default="999999")
	ap.add_argument("--wrong", nargs="*", default=["000000", "123456", "abcdef", "999998", ""])
	ap.add_argument("--no-timing", action="store_true")
	args = ap.parse_args()
	image = open(args.image, "rb").read()
	failed = []

	def check(what, good):
		print("keybag-offline: %s %s" % ("ok  " if good else "FAIL", what))
		if not good:
			failed.append(what)

	records = find_records(image)
	check("the bag's records are on the raw volume, their digests hold", len(records) >= 1)
	if not records:
		return 1
	newest = max(records, key=lambda r: r["generation"])
	print("keybag-offline: generation %d at %#x: Argon2id %d KiB x %d, tiers %#x" % (newest["generation"], newest["offset"], newest["mem"], newest["passes"], newest["tiers"]))
	check("a passcode is set", newest["has_passcode"] == 1 and newest["migrating"] == 0)
	check("the test key is not on the volume in the clear", TEST_KEY not in image and TEST_KEY[:16] not in image)
	check("nor is the passcode anywhere near the record", args.passcode.encode() not in image[newest["offset"]:newest["offset"] + SLOT])
	for code in args.wrong:
		check("wrong passcode %r brings no key" % code, roots_of(code.encode(), newest) is None)
	roots = roots_of(args.passcode.encode(), newest)
	check("the right passcode brings the roots (Argon2id and key wrap are the standard ones)", roots is not None and set(roots) == set(TIERS))
	if roots:
		where = opens_a_blob(image, class_key(roots["A"], CLASS_AK))
		check("and a class ak blob the kernel wrapped in an earlier boot opens to the test key" + (" (at %#x)" % where if where is not None else ""), where is not None)
		check("one class's key is not another's", class_key(roots["C"], CLASS_CK) != class_key(roots["A"], CLASS_AK))
	if not args.no_timing:
		mem, passes = 65536, 3
		trial = dict(newest, mem=mem, passes=passes)
		started = time.time()
		kek_of(b"000000", trial)
		one = time.time() - started
		print("keybag-offline: one guess at the phone's cost (Argon2id %d KiB x %d) on one core here: %.3f s; all 10^6 six-digit codes: %.1f core-hours, the 26^8 lowercase eight-letter ones: %.0f core-years"
		    % (mem, passes, one, one * 1e6 / 3600, one * 26 ** 8 / 3600 / 24 / 365))
	print("keybag-offline: %s" % ("FAILED: " + "; ".join(failed) if failed else "all checks passed"))
	return 1 if failed else 0


if __name__ == "__main__":
	sys.exit(main())
