# The real key store

**On by default on the Galaxy S9 (`starlte`), off everywhere else.** `keystore_real=0` turns it off on the S9, `keystore_real=1` turns it on elsewhere. The S9's default is compiled in (`HAKIOS_DEVICE` from `CONFIG_DEVICE`, in `hakios.drivers.h`), because its boot line has no room for another argument. With it off the kernel behaves as [the passcode](passcode.md) describes: the passcode gates the screen and
encrypts nothing. With it on, the keychain's keys are protected by the passcode. This page says what that means and where it stops. This is
not Apple-grade security: there is no Secure Enclave, and today no key that lives outside the kernel.

**In short: this protects keychain items, not files. There is no hardware key yet, so a 6-digit passcode falls to anyone who reads the flash and guesses offline. A forgotten passcode loses the keychain.**

**Status.** Seen in QEMU and on a Galaxy S9 on real `/private/var` data on USERDATA, through Settings and the lock screen (see Testing). Whether class A items refuse while the phone is locked has not been checked on a phone.

## What an attacker gets

Read this before relying on anything below. "Keychain" means items whose class is A, C or "when passcode set". **Ordinary files are not
encrypted at all**: the data volume is plain HFS Plus, so photos, messages and mail databases are readable from the flash whatever the
passcode (see stage 4).

| Attacker | Today (no `keystore_real`) | Passcode entanglement (this page) | With a hardware-bound secret (TEE route, not available yet) |
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
- **Entitlement checks are not a wall against root.** This kernel checks no code signatures, so a program can list any entitlement in its own
  signature. An entitlement therefore counts only if the program also runs from a read-only volume. That stops a process that only holds a user
  client, or starts something it wrote to `/private/var`. It does not stop root, which can attach to a daemon that holds the entitlement, or write to the flash.
- **Old key material may survive on the flash.** Changing the passcode overwrites both slots of the file, but the flash's own remapping may
  keep older copies. An old record falls to its old passcode, or to no passcode at all if it dates from before one was set. Apple solves this
  with effaceable storage; there is none here. A device that has held data under a weak or no passcode should be reset (see below), not just
  given a better one.
- **Residue.** Secrets this code holds are wiped, but the passcode also passes through SpringBoard, Settings and `passcoded`, and an unwrapped key through IOKit's own buffers on the way to securityd. Neither is wiped here.
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

The answers are what securityd's `ks_crypt` expects (read from 16.5's securityd, and seen on the phone): locked is `0xe00002e2`, shown as
`errSecInteractionNotAllowed`; no such key is `0xe00002f0`, `errSecNotAvailable`; a blob that will never open is `0xe00002bc` (`kIOReturnError`), `errSecDecode`,
and **securityd then drops the item**. That last one is why a damaged bag answers "locked" and not "never": the items must survive until
someone decides. AKS's own `0xe007c008` is the wrong answer here: securityd reports it as `errSecNotAvailable` and keeps the item, so every read of it fails again, and `profiled` gives up changing the passcode when it cannot read its own keychain item.

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
   Booting an older kernel, or `keystore_real=0`, afterwards shows no passcode, and finds the new keychain unreadable: **the screen opens, the keychain does not.** A passcode set meanwhile in the old file holds when the key bag is back, if the bag has none (over the bag's keys, which stay); if the bag has one, the bag's holds and the old file is overwritten.
5. **Unlocking gets slower**: one Argon2id per attempt, single-threaded in the kernel. Not yet measured on the S9; the log prints `Argon2id N KiB x P: M ms`
   and the cost is stored per bag, so it can be tuned after a phone run. The target is under about a second. While it runs, 64 MiB of memory is in use.
6. **Stage 4 (files) would need `/private/var` reformatted** and everything copied back or lost, and would cost I/O speed (below).
7. **Daemons will see real lock behaviour**: class A items refuse while locked, class C items refuse before the first unlock. That is iOS's contract, but this
   userland has never run under it here, so expect bugs on the phone.

## TWRP, wipes and installers

The keys live in `/private/var/hakios-keybag`, on USERDATA, and nowhere else: the boot image and recovery hold none, so flashing either changes nothing in the bag.

| What | What happens |
|---|---|
| Going through TWRP and back | Nothing. TWRP cannot mount HFS Plus (`Failed to mount '/data'`), does not ask to decrypt, and writes nothing: the first 16 MiB of USERDATA hashed the same across a TWRP restart, and the phone came back locked with its passcode. Seen on phone 2 |
| Flashing a boot image | The bag stays. A `passcode_reset=1` image destroys the keys (above); a `keystore_real=0` or older image shows no passcode and cannot read the keychain |
| Installer zip | Writes SYSTEM and BOOT only: the bag stays. The `-U` format-userdata zip destroys it with the rest of USERDATA. As the installer is described; not checked or run for this |
| TWRP's Wipe, Factory Reset, Format Data | Not tried: they would overwrite USERDATA, bag and all. With no HFS Plus to mount, `/private/var` comes up in memory: no passcode and nothing kept, until the volume is made again. Don't use them unless that is the point |
| A TWRP backup of data | Not tried; with data unmountable there should be nothing to back up. A raw copy of the partition holds the bag, which is the offline guessing row of the table above |

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
| Escrow bags (`com.apple.keystore.escrow.create`, `aks_create_bag` 0x02, `aks_load_bag` 0x06, backup bags) | lockdownd pairing, MDM, backups | not done; those selectors answer unsupported and `aks_unlock_bag` 0x0c is refused, so nothing is unlocked by a bag |
| Effaceable storage, `obliterate-d-key` | `mobile_obliterator`, "Erase Data" | not done. **Do not turn on "Erase Data"**: untested here |
| Secure Enclave keys, attestation (`sik`) | CryptoTokenKit, 65 programs hold the entitlement | not done and no plan: no Secure Enclave |

### Entitlements

As carried by 16.5's binaries, from scanning the root volume's code signatures. The kernel's table is `kKSAccess` in `MiniAppleKeyStore.cpp`; selectors not in it are open. **The default is log-only**
(`keystore_entcheck=1`): which programs really call what can only be seen on a phone, and `keystore_entcheck=2` must wait for that list. `passcoded` holds `com.hakios.keystore.change`, this port's own, for its selectors 0x100 and 0x101, as well as needing root.

**`keystore_entcheck=2` would break today's phones.** In the one phone run the log said it would refuse UserEventAgent (0x66, no `com.apple.keystore.device`), GSSCred (0x0a, no `.access-keychain-keys`), and `passcoded` itself: that ramdisk's `passcoded` was unsigned. `userland/build.sh` now signs it (ldid, or a Mac's `codesign`), but a ramdisk made by hand has to as well, and a signed `passcoded` has not been run on a phone. The list is a sample: these lines reach only the kernel's last log, seen for parts of a few boots. The phone's ramdisk root counting as read-only is also unchecked. mobileactivationd (0x2b) and sharingd (0x39) called selectors that are not implemented.

| Entitlement | Selectors | Holders |
|---|---|---|
| `com.apple.keystore.device` | unlock 9, lock 8 and 0x20 | SpringBoard, Preferences, coreauthd, keybagd and others: 45 programs |
| `com.apple.keystore.device.verify` | verify 0x2a | coreauthd, keybagd: 7 programs |
| `com.apple.keystore.access-keychain-keys` | wrap 0x0a, unwrap 0x0b | securityd, sharingd, profiled, coreauthd: 14 programs |
| `com.apple.keystore.lockassertion` | assertions 0x1a, 0x1b | 17 programs |
| `com.apple.private.applecredentialmanager.allow` | the ACM user client (stage 3) | securityd, coreauthd, keybagd: 23 programs |

### The AppleCredentialManager protocol (for stage 3)

`SharedUtils` (in the dyld shared cache) exports `ACM*` and `LibCall_ACM*`, each of which serializes its arguments and hands a command to a transport; coreauthd's is IOKit (`ioKitTransport`, the service `AppleCredentialManager`), and securityd's and keybagd's use the same client library.

A command is built by `LibCall_BuildCommand`: the magic `DRCS`, a byte of protocol version, a byte of **command**, a byte of flags, a reserved byte, then the serialized payload. `LibSerialization`'s exported `Serialize*` and `Deserialize*` give each payload's layout. Commands, read from each `LibCall_` function:

| Command | Function |
|---|---|
| 0x24, falling back to 0x01 | context create; answers a 17- or 21-byte handle |
| 0x25, falling back to 0x12 | context create from external form |
| 0x02 | context delete |
| 0x03 | verify policy, with requirements |
| 0x05 | add credential with scope |
| 0x06, 0x09 | remove credentials, by value and by type, with scope |
| 0x21, 0x1b | credential property, in a context and in the global context |
| 0x28, 0x29 | context data, set and get |
| 0x2e | context info |
| 0x19, 0x2c | environment variable, get and set |
| 0x1a | kernel control |
| 0x1c | double-click notification |
| 0x1f | ACL auth method |
| 0x11 | biometry availability |
| 0x26, 0x2d, 0x30, 0x31 | TRM state, trusted accessories, context load and unload |

Credential kinds include passcode validated, passphrase entered and extractable, biometry matched, user output displayed.

A stand-in has to do at least context create and delete, add a passcode credential (checked against the key bag, and counted like any attempt), verify a policy that asks for a passcode, credential properties, data and environment variables. **It is not written.** It can only be tested against coreauthd on a phone or in a QEMU run with a full userland, and neither is available yet, so writing it blind would be guesswork. Keep Face ID's rules in mind: without a Secure Enclave, a biometric unlock must keep class A's key recoverable in kernel memory while locked.

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

Nothing in the key store depends on the TEE yet: `hwTangle()` in `MiniAppleKeyStore.cpp` is still a copy, and it is the one place this would go in. Changing it makes existing bags unreadable. No keys are extracted from firmware, and EL3 and the TEE are not patched.

Kinibi starts under HakiOS on a Galaxy S9 (`tee_probe=2`: state 2, one boot on one phone). Beyond that, `drivers/core/hakios.exynos_tee.c` has two test modes, off unless asked. **Neither has run on a phone:**

- `tee_probe=3`: asks Kinibi its version over MCP, then opens and closes a session on a harmless TA read from the phone's own VENDOR partition.
- `tee_probe=4`: also runs Samsung's keymaster TA (`ffffffff00000000000000000000003e`): configure, generate an HMAC key, HMAC a fixed test value twice. The TA seals the key into a blob, kept in `/private/var/hakios-tee-km-test.blob`; a later boot reuses that file.

What the plan is, once those have answered: Argon2id's output goes through an HMAC under such a key, so a guess needs this phone's TA and can't be run on someone else's machine. The bag would be marked as tangled when it's created, and old bags stay as they are. If the TEE doesn't start, a tangled bag stays locked and nothing is overwritten: that's the same fail-closed path as a damaged bag, so a boot with the TEE broken keeps the keychain shut but doesn't lose it, while losing the blob file would. It stays behind its own boot argument.

What it would and wouldn't do, some of it not yet tested:

- **Would:** stop guessing off the phone, if the blob can't be opened elsewhere. That's inferred from the TA's use of `tlApiDeriveKeyWithREK`; it hasn't been shown.
- **Wouldn't:** stop a modified kernel from capturing the passcode as it's typed, or from calling the TA with guesses at the TEE's speed. There's no retry limit outside the kernel without Samsung's gatekeeper TA, which isn't approved here.
- **Unknown:** whether the blob survives an OS version or boot state change, whether it's bound to boot state, and whether the TA's ICCC code writes anything that persists. The configure call here leaves out the TIMA measurement file because the real HAL's path with it reaches an ICCC flag write. A published analysis of Samsung's keymaster blobs (Shakevsky et al., 2022) found weaknesses in older blob formats. Whether they apply to this TA's version hasn't been checked.

## Boot arguments

| Argument | Meaning |
|---|---|
| `keystore_real=N` | the key bag. 1 on, 0 off. Without it: on for the S9 (`starlte`), off for the rest |
| `keystore_entcheck=N` | who may call what. 0: no check; 1 (the default): log what would be refused, and allow it; 2: refuse |
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

`... stale` covers a bag with no passcode meeting a passcode in the old file: it holds, and what the bag's keys wrapped still opens.

`... entitlements` boots a test program signed with `com.apple.keystore.device` alone, then unsigned: the table allows unlock and refuses wrap and changing the passcode, then refuses all but the state.

After boot 11, `tools/keybag-offline.py` reads the raw volume as a thief would, with the reference Argon2 and OpenSSL's key wrap: the test key and the passcode are not on the volume in the
clear, wrong codes bring nothing, and the right code brings the roots and opens a blob the kernel wrapped. It needs `pip install argon2-cffi cryptography`.

QEMU runs the kernel's logic and HFS only: no securityd, no lock screen. The self-test uses a cheap Argon2id (`keystore_kdf_kib=1024`, one pass). It says nothing about behaviour on the phone
or the entropy available at first boot.

On a Galaxy S9, seven boots with `/private/var` in memory (USERDATA not mounted), through the real lock screen:

- Argon2id at 64 MiB and 3 passes: 294 ms to set a passcode, 302 to 369 ms for each unlock or wrong code. No watchdog panic.
- locking, a wrong code ("no keys") and the right code ("the class keys are in memory") all behave.
- no daemon crashed in about 200 s locked. searchd aborted about 1 s after the first unlock, as it also has on boots without the key store. backboardd died twice: once at its old Mesa signature, once with `-[NSNull applyUIMode:]`, not seen before and not explained.

On phone 2 on 4 November, `/private/var` on USERDATA with an existing keychain, by console taps through Settings and the lock screen: a 6-digit passcode set (Argon2id 471 ms), Settings shows Turn Passcode Off, the keypad shows after a lock, a wrong code is refused and counted (287 to 296 ms), the right one opens (about 290 ms); after a restart through TWRP the keypad says "iPhone requires your passcode after restarting", and before the first unlock securityd got "locked" for class 7, 83 times in that boot, and no daemon dropped an item; then unlocked, changed, and turned off. Old items were dropped by securityd, which made new metadata keys.

Not checked on a phone: a class A item answering -25308 while locked (no daemon asked for one in 25 s locked, and the console has no shell to run `keychain_test` from), a keychain item surviving a restart, a passcode from the old file carried over (QEMU only), and the entropy at first boot.
