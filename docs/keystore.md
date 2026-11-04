# The real key store (`keystore_real=1`)

**Off by default.** Without `keystore_real=1` the kernel behaves as [the passcode](passcode.md) describes: the passcode gates the screen and
encrypts nothing. With it, the keychain's keys are protected by the passcode. This page says what that means and where it stops. This is
not Apple-grade security: there is no Secure Enclave, and today no key that lives outside the kernel.

**Status.** Proven in QEMU only (see Testing). It has not run on a phone, and Apple's daemons have not yet run on it. Do not turn it on for
data you care about.

## What an attacker gets

Read this before relying on anything below. "Keychain" means items whose class is A, C or "when passcode set". **Ordinary files are not
encrypted at all**: the data volume is plain HFS Plus, so photos, messages and mail databases are readable from the flash whatever the
passcode (see stage 4).

| Attacker | Today (no `keystore_real`) | Passcode entanglement (this page) | With a hardware-bound secret (TEE route, not available) |
|---|---|---|---|
| App sandbox escape to root, phone unlocked | everything | everything: root can read the keychain through securityd, or read memory, and can read the key bag file | the same while unlocked |
| Root, phone locked since its last unlock | everything | class A and "when passcode set" items stay shut: their roots are not in memory. Class C items and all files are readable. Root can copy the key bag and guess offline (next row) | offline guessing stops; guesses go through the TEE, which could enforce the limits |
| Stolen phone, powered off, flash read out | everything | class D: everything. Classes A, B, C, P: an offline guess costs one Argon2id. A **6-digit code falls** (about 41 core-hours at today's cost, on one measured server core: a few dollars of rented machines). A **long random alphanumeric does not** (10 random letters and digits: about 10^10 core-years) | guessing needs this phone's TEE, so a 6-digit code could hold, if the TEE route opens |
| Can flash a modified kernel, one visit (the bootloader is unlocked, so anyone holding the phone can) | everything | nothing more than the row above from the flash alone: the keys are not on it. Retry limits are gone, but offline guessing had none anyway | the same for guessing |
| Can flash a modified kernel, and the owner then uses the phone | everything | **everything**: the modified kernel records the passcode when it is typed. Nothing here can detect this | still everything, unless keys are tied to the boot state, which is unverified |

Limits to keep in mind:

- **Wrong-attempt limits are software only.** The count and the delays are in a file: a kernel of the attacker's, or someone rewriting the
  flash, resets them, and a copy of the bag is attacked with no limits at all. Without a hardware secret they only slow someone who is using
  this phone's own lock screen.
- **Entitlement checks are not a wall against root.** This kernel checks no code signatures. Stage 2 helps against a process that merely
  holds a user client; it does not stop root, which can attach to a daemon that holds the entitlement.
- **Old key material may survive on the flash.** Changing the passcode overwrites both slots of the file, but the flash's own remapping may
  keep older copies. An old record falls to its old passcode, or to no passcode at all if it dates from before one was set. Apple solves this
  with effaceable storage; there is none here. A device that has held data under a weak or no passcode should be reset (see below), not just
  given a better one.
- **Random numbers.** The loader's boot seed is derived only from a timer (`xnuloader/loader.c`), which is weak. The kernel's generator also takes
  interrupt timing, this file stirs in timing jitter, and from the second boot on it stirs in a seed saved in the bag. How much entropy that
  gives on the first boot of a phone has **not been measured**. It matters most at that first boot, when the keys are made.

## How it works

```
passcode ─ Argon2id(salt, 64 MiB, 3 passes, one lane) ─ hwTangle (today: nothing) ─ HMAC-SHA-256 ─ KEK
KEK ─ AES-256 key wrap (RFC 3394) ─ roots of tiers A, B, C, P, in /private/var/hakios-keybag
root + class number ─ HMAC-SHA-256 ─ the key of that class
class key ─ AES-256 key wrap ─ the keys securityd asks to have wrapped (selectors 0x0a, 0x0b)
```

RFC 3394's integrity check is the only verifier: there is nothing to compare with, so a wrong passcode yields no key and nothing else.

| Tier | Classes (AppleKeyStore's numbers) | Key available |
|---|---|---|
| A | 1 (file A), 6 `ak`, 9 `aku` (keychain "when unlocked") | while unlocked; wiped at each lock |
| B | 2 (file B) | as A for now. iOS's B wraps with a public key while locked, so a file can be created; that belongs with stage 4 |
| C | 3 (file C), 7 `ck`, 10 `cku` ("after first unlock") | from the first unlock of a boot until the restart |
| P | 12 `akpu` ("when passcode set") | as A, and only while there is a passcode; destroyed when it is removed, so its items are lost, as on iOS |
| D | 4 (file D), 8 `dk`, 11 `dku` ("always") | always. **In the clear in the file**: nothing protects it, it can only be thrown away |

With no passcode, A, B and C are wrapped under a key derived from D's root: in effect, in the clear. That is honest about what "no passcode" means.

The answers are what securityd's `ks_crypt` expects (read from 16.5's securityd): locked is `0xe00002e2`, shown as
`errSecInteractionNotAllowed`; no such key is `0xe00002f0`, `errSecNotAvailable`; a blob that will never open is `0xe007c008`, `errSecDecode`,
and **securityd then drops the item**. That last one is why a damaged bag answers "locked" and not "never": the items must survive until
someone decides.

**Damage fails closed.** A key bag file that exists but has no valid slot, or whose keys do not come out with no passcode set, leaves the phone
locked: no passcode opens it, no class opens (D's root was in the file too), and nothing overwrites it. A file that does not exist is a new bag with
new keys, so deleting it gets an attacker nothing: what was wrapped before stays shut. Writes still go to two slots with a generation number and a
SHA-256, so a torn write leaves the previous one.

The attempt is counted and written before the key derivation runs, so cutting the power during one does not make it free.

**The primitives are not written here.** The kernel has no corecrypto AES: Apple's kext is closed,
and `osfmk/minicrypto/mc_aes.c` is a byte-table AES that is not constant-time. So: BearSSL's constant-time `aes_ct64` (AES) and Monocypher 4.0.2
(Argon2id) are vendored unchanged in `xnu/files/osfmk/minicrypto/third_party/`, and `mc_keywrap.c` is the glue. FIPS 197, RFC 3394 and RFC 9106 known
answers run before any bag is used; failing them is treated as damage. `tools/keybag-offline.py` checks the result from outside the kernel with
the reference Argon2 and OpenSSL.

## Consequences (the owner decides)

Turning this on means:

1. **A forgotten passcode loses the protected data.** There is no recovery by anyone, including us. Today `passcode_reset=1` recovers everything.
2. **`passcode_reset=1` now removes the lock by destroying the keys.** The phone opens with no passcode; every keychain item of classes A, B, C, D and P
   is lost, and securityd drops them as it meets them. Files stay, because files are not encrypted yet.
3. **The existing keychain is dropped.** Items wrapped by the old reversible scheme are refused (`errSecDecode`), so securityd deletes them: saved Wi-Fi
   passwords, account tokens. The rest of `/private/var` needs no reformat for stages 1 to 3.
4. **An existing passcode carries over** at the first unlock, and the old verifier file is overwritten: it was a far cheaper way to test guesses than the bag.
   Booting an older kernel afterwards shows no passcode, and finds the new keychain unreadable.
5. **Unlocking gets slower**: one Argon2id per attempt, single-threaded in the kernel. Not yet measured on the S9; the log prints `Argon2id N KiB x P: M ms`
   and the cost is stored per bag, so it can be tuned after a phone run. The target is under about a second. While it runs, 64 MiB of memory is in use.
6. **Stage 4 (files) would need `/private/var` reformatted** and everything copied back or lost, and would cost I/O speed (below).
7. **Daemons will see real lock behaviour**: class A items refuse while locked, class C items refuse before the first unlock. That is iOS's contract, but this
   userland has never run under it here, so expect bugs on the phone.

## What iOS 16.5 expects, and where this stands

From 16.5's binaries (securityd, keybagd, coreauthd, SharedUtils, MobileKeyBag, every entitlement on the root volume) and Apple's public Security sources.

| Contract | Who relies on it | Status |
|---|---|---|
| Classes A, C, D, "this device only", "when passcode set" for wrapped keychain keys | securityd: `aks_wrap_key` 0x0a (scalars: bag, class; key in, wrapped out, class out), `aks_unwrap_key` 0x0b | **stage 1** |
| Lock wipes A; first-unlock semantics of C; D always | securityd, every daemon reading keychain items | **stage 1** |
| Passcode entanglement, wrong code gets no key | everything above | **stage 1**, without hardware entanglement |
| Fail closed, zeroise secrets | | **stage 1** |
| Who may call what (entitlements below) | `com.apple.keystore.*` | **stage 2** |
| ACL keychain items (`SecAccessControl`, biometry, "user presence"): `aks_ref_key_*` on selector 0x2b, DER, with an ACM context | securityd `ks_encrypt_acl` | not done: 0x2b answers unsupported, so such items cannot be created. Needs stage 3 |
| AppleCredentialManager: contexts, credentials, policies, environment variables (`ACMContextCreate` 0x24 or 1, ...), via user client selector 0 | coreauthd, securityd, keybagd (`com.apple.private.applecredentialmanager.allow`) | not done, stage 3. The commands are exported by name from `SharedUtils`; the map is below |
| `LAContext`, passcode as a credential, biometric policies | Settings, apps, Face ID enrolment | needs stage 3 |
| Biometric unlock only after a first unlock, passcode again after 48 hours, a reboot or 5 failed matches | iOS's rule | not done, stage 3. Without a Secure Enclave, holding class A's key for a biometric unlock means **keeping it in kernel memory while locked**, weaker than iOS; the owner's call |
| File classes A, B, C, D on `/private/var` (HFS+ `cprotect`) | every app and daemon's files | **off today**: `xnu/hfs/hfs.patch` sets `CONFIG_PROTECT 0`. Stage 4 |
| Escrow bags (`com.apple.keystore.escrow.create`, `aks_create_bag` 0x02, `aks_load_bag` 0x06, backup bags) | lockdownd pairing, MDM, backups | not done; those selectors answer unsupported, so nothing is unlocked by a bag |
| Effaceable storage, `obliterate-d-key` | `mobile_obliterator`, "Erase Data" | not done. **Do not turn on "Erase Data"**: untested here |
| Secure Enclave keys, attestation (`sik`) | CryptoTokenKit, 65 programs hold the entitlement | not done and no plan: no Secure Enclave |

### Entitlements

As carried by 16.5's binaries, from scanning the root volume's code signatures.

| Entitlement | Selectors | Holders |
|---|---|---|
| `com.apple.keystore.device` | unlock 9, lock 8 and 0x20 | SpringBoard, Preferences, coreauthd, keybagd and others: 45 programs |
| `com.apple.keystore.device.verify` | verify 0x2a | coreauthd, keybagd: 7 programs |
| `com.apple.keystore.access-keychain-keys` | wrap 0x0a, unwrap 0x0b | securityd, sharingd, profiled, coreauthd: 14 programs |
| `com.apple.keystore.lockassertion` | assertions 0x1a, 0x1b | 17 programs |
| `com.apple.private.applecredentialmanager.allow` | the ACM user client (stage 3) | securityd, coreauthd, keybagd: 23 programs |

### The AppleCredentialManager protocol (for stage 3)

`SharedUtils` (in the dyld shared cache) exports `ACM*` and `LibCall_ACM*`, each of which marshals its arguments and calls a transport block with a command number; coreauthd's
transport is IOKit, selector 0. `ACMContextCreate` sends command 0x24 (falling back to 1) and takes back a 17- or 21-byte context handle. A stand-in has to implement at least
context create and delete, add credential with scope, verify policy (with requirements), get and set data, credential properties, and environment variables. It can only be
tested on a phone, or in a QEMU run with a full userland: there is no such test yet.

## Stage 4: files (feasibility)

Possible, large, and not started.

- HFS's side exists: `bsd/hfs/hfs_cprotect.c` is compiled but disabled by `CONFIG_PROTECT 0`. XNU's `bsd/vfs/vfs_cprotect.c` gives `cp_register_wraps` (new key, unwrap, rewrap, invalidate,
  backup) and `cp_key_store_action` (lock events), which the key store must drive.
- **The storage driver does the encryption**: HFS hands it the file's key and offset on each buffer (`bufattr_cpx`, `cpx_iv_aes_ctx`) and an iPhone's hardware does the AES. Here
  `drivers/storage/hakios.ufshc.c` would need a software AES path with bounce buffers, or Samsung's inline FMP engine in the UFS host, which nothing here drives. Bitsliced AES is far too slow
  for a data path (order of 100 MB/s per core, measured on nothing yet); the CPU's AES instructions would be the way, which needs the kernel to save vector registers around them.
- **The volume must be formatted for it**: `kHFSContentProtectionMask` in the volume header. `/private/var` as it exists would be copied out and back, or reset.
- **Boot order**: with file class C as the default, nothing written that way is readable before the first unlock. iOS's restore marks what boot needs as class D. This userland's
  `var.skel` does not, so getting to the lock screen at all needs that list worked out, and a mistake means a phone that does not boot to the lock screen.
- Class B needs the asymmetric part (X25519 is in the vendored Monocypher).

## The TEE route

Samsung's keymaster and gatekeeper trusted applications exist on the phone and, from strings and light disassembly only, offer a device-bound key derivation and an external retry
count. Nothing has been run. It needs a Kinibi driver for XNU first (weeks), may depend on Knox state, and writes to RPMB cannot be undone. `hwTangle()` in `MiniAppleKeyStore.cpp` is the
one place it would go in; until then it adds nothing. Changing it makes existing bags unreadable. No keys are extracted from firmware and EL3 and the TEE are not patched.

## Boot arguments

| Argument | Meaning |
|---|---|
| `keystore_real=1` | the key bag. Off without it |
| `keystore_kdf_kib=N` | Argon2id's memory, for the self-test only: ignored without `passcode_selftest` |
| `passcode_reset=1` | with `keystore_real=1`: destroys the keys, leaves no passcode, overwrites the old verifier file |

## Testing

`qemu/run-passcode-test.sh real` boots the kernel's self-test five times on one HFS Plus volume; `... migrate` covers the old file. What it proves:

- a locked key store cannot wrap or unwrap class A or P, and their roots are zero in memory;
- a wrong passcode brings no key and counts; the right one brings back the same keys, across restarts, and blobs from an earlier boot open;
- class C is shut before the first unlock and stays open after a lock; D is always open;
- a damaged, other-class or old-style blob is refused;
- both slots damaged: locked, nothing opens, the right passcode gets nothing and is not counted, nothing is overwritten;
- `passcode_reset=1` gets out of that, and every old blob then answers "never".

After boot 11, `tools/keybag-offline.py` reads the raw volume as a thief would, with the reference Argon2 and OpenSSL's key wrap: the test key and the passcode are not on the volume in the
clear, wrong codes bring nothing, and the right code brings the roots and opens a blob the kernel wrapped. It needs `pip install argon2-cffi cryptography`.

QEMU runs the kernel's logic and HFS only: no securityd, no lock screen. The self-test uses a cheap Argon2id (`keystore_kdf_kib=1024`, one pass). It says nothing about behaviour on the phone,
unlock time on the S9, or the entropy available at first boot.
