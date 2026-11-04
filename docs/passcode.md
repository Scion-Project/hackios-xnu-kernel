# The passcode

Settings > Face ID & Passcode (Touch ID & Passcode on some images) can turn a passcode on, change it and
turn it off; the lock screen then asks for it after a lock and after a restart, refuses a wrong one,
and makes you wait after too many wrong ones, with iOS's own delays.

**What it is not.** There is no Secure Enclave and no hardware key in this port. The passcode gates
the user interface (the lock screen, Settings, ManagedConfiguration's checks) and nothing else. No
data is encrypted with it: the data volume is plain HFS Plus, the keychain's key "wrapping" is a
reversible transformation, and the verifier file can be removed by root or by anyone holding the
flash. It stops someone who picks the phone up; it does not protect data from someone who takes it
apart or boots something else.

With `keystore_real=1` (off by default, seen on a phone only with `/private/var` in memory) the keychain's keys are protected by the passcode instead: see [the real key store](keystore.md). This page describes the kernel without it.

## How it works

| Piece | What it does |
|---|---|
| `xnu/files/iokit/Kernel/MiniAppleKeyStore.cpp` | The kernel's stand-in for AppleKeyStore. Keeps the verifier, the lock state, the failed attempts and their delay, the grace period and the passcode's metadata; answers MobileKeyBag's selectors. |
| `userland/src/passcoded.m`, `com.hakios.passcoded.plist` | The stand-in for keybagd's XPC service `com.apple.mobile.keybagd.xpc` (Apple's keybagd stays out: it would look for a Secure Enclave). Changing the passcode and reading its metadata go through it to the kernel. It also posts keybagd's `lock_status` and `first_unlock` notifications. |
| `/private/var/hakios-passcode` | The record: two 4 KB slots, written alternately with a generation number and a SHA-256, synced. On the phone it is on USERDATA and survives restarts; on a RAM boot and in QEMU it lasts until the next start. |

The verifier is PBKDF2-HMAC-SHA256 of the passcode with a random 16-byte salt and 100000 iterations,
compared in constant time. The kernel checks its PBKDF2 against RFC 7914's test vector first and will
not set or check a passcode if that fails.

Lock states (what `MKBGetDeviceLockState` returns): 0 unlocked, 1 locked, 3 no passcode. With a passcode
the phone starts locked; a lock locks at once. The grace period Settings' "Require Passcode" sets is kept and reported but not
applied: whatever it says, the passcode is asked for at once, which is sooner than chosen and never later.

Lock assertions (`MKBDeviceLockAssertion`, AppleKeyStore 0x1a: "keep the device unlocked") are granted only while it is
unlocked. SpringBoard asks for one to decide whether the passcode is needed ("Checking if passcode locked" in
`SBFUserAuthenticationController`); granting it while locked let the lock screen in without its keypad. Holding one does not
keep the device from locking here.

Who may do what: changing the passcode is for root (passcoded, selector 0x100). Trying one is open to any program, as nothing
here checks entitlements, so a program can run up the failed attempts and their delay.

Wrong passcodes: the delays of Apple's Platform Security guide. 1 to 4 failed attempts: none; the 5th:
1 minute; the 6th: 5 minutes; the 7th and 8th: 15 minutes; the 9th and every one after it: 1 hour.
Never longer, never "permanently blocked", never a wipe. Attempts while a delay runs are refused and not
counted. The count and the delay are kept in the record, so a restart does not reset them. Do not turn
on Settings' "Erase Data": SpringBoard has its own wipe logic for it, and this port does not support a
wipe (`obliteration` is kept out of the jobs, but nothing here has tested what SpringBoard does).

## Escape hatches

If the lock screen ever cannot take the passcode (it was forgotten, the keypad does not show, a bug):

1. **Boot argument `passcode_reset=1`.** Flash a boot image that has it (TWRP or download mode work whatever
   the lock screen does). At the first call to the key store of that boot the stored passcode
   is discarded: both slots of `/private/var/hakios-passcode` are overwritten with a "no passcode"
   record, and the kernel log says `passcode_reset=1: the stored passcode is discarded`. Then flash the
   normal image again. Nothing else in `/private/var` is touched. The kernel's command line holds 607 characters, the
   loader adds about 150 of its own, and what does not fit is cut silently from the end. The Galaxy S9's daily arguments
   leave no room: an appended `passcode_reset=1` was cut, and the passcode stayed. Make room by taking Wi-Fi out of that one
   image: `devices/starlte/mkimage.sh ... wifi= wifi_pcie= brcmfmac= brcmfmac_poll= brcmfmac_tcm_idx= passcode_reset=1`.
2. **Delete the file.** With the data volume mounted somewhere else (USERDATA is HFS Plus; TWRP does not
   mount it, a Mac or Linux with hfsplus does), delete `hakios-passcode` at the root of the volume.
   A missing, unreadable or damaged file means "no passcode".

A kernel that cannot read the file, or whose key derivation fails its self-test, treats the phone as
having no passcode: open, never shut for good.

## What calls what (Darwin userland 16.5)

Set, change, turn off: Settings (`DevicePINController setPIN:completion:`) asks
`MCProfileConnection changePasscodeFrom:to:outError:`; ManagedConfiguration's `profiled` calls
`MKBKeyBagChangeSystemSecret` (or `...WithEscrow`), which MobileKeyBag sends over XPC to
`com.apple.mobile.keybagd.xpc` as `changeSystemSecretfromOldSecret:oldSize:toNewSecret:newSize:opaqueData:reply:`
(the secrets as file handles; the opaque data is MC's plist of the passcode's kind: `keyboardType`,
`simpleType`). passcoded passes it to the kernel (this port's selector 0x100).

Which keypad: `MCPasscodeManager _publicPasscodeDict` reads `MKBKeyBagCopySytemSecretBlob`, XPC
`getPasscodeBlobWithReply:`, the kernel's 0x101.

Is there a passcode: `MCPasscodeManager isPasscodeSet` is `MKBGetDeviceLockState() != 3`.

Verify (Settings asks for the old passcode): `DevicePINController validatePIN:` →
`MCProfileConnection unlockDeviceWithPasscode:outError:` → `MKBUnlockDevice`.

Lock screen: SpringBoardFoundation's `SBFMobileKeyBag` calls `MKBUnlockDevice` (AppleKeyStore selector
9: the handle, the passcode's address and length as scalars), `MKBLockDevice` (8, or 0x20 for "now"),
`MKBGetDeviceLockStateInfo` (0x23 or 0x11: DER `sls` lock state, `ss` flags, `sb` seconds of delay
left, `sfa` failed attempts; SpringBoard shows "try again in" from `sb`), `MKBGetDeviceConfigurations`
(0x1f: `gp` grace period, `mua` maximum attempts), and `MKBEventsRegister`, an IOGeneralInterest
notification on AppleKeyStore: message 0xe3ffd00d "status changed", 0xe3ffd00f "first unlock".
`MKBUnlockDevice` also tells keybagd `passcodeUnlockStart/Success/Failed` over XPC.

LocalAuthentication's coreauthd checks a passcode with AppleKeyStore's 0x2a (`aks_verify_password`:
a packed list of the passcode and a context), which the kernel answers the same way. Everything else
coreauthd does goes through AppleCredentialManager (ACMLib in `SharedUtils`: a user client of the
`AppleCredentialManager` service, selector 0 for serialized commands, 2 for context creation), which
this port does not have yet. Face ID enrolment needs it: see below.

## Towards Face ID

BiometricKit's enrolment asks for the passcode and takes it as an ACM credential (LocalAuthentication's
`LAContext` with a passcode credential, coreauthd, AppleCredentialManager). What the iris work will
need from here:

- an `AppleCredentialManager` stand-in in the kernel that accepts a passcode credential, checks it with
  the key store's verifier (the same PBKDF2 path as selector 0x2a) and satisfies a policy that asks for
  one; its command protocol is ACMLib's (`LibCall_ACMContextCreate`, `...AddCredentialWithScope`,
  `...VerifyPolicyEx`, `...GetExternalForm`), to be mapped from SharedUtils;
- a "biometric unlock" call into the key store for the matcher (unlock without the passcode, after it has
  been entered once since boot, as iOS requires), refused before the first unlock of a boot;
- the same rule for Face ID as for the passcode: it opens the screen, it protects no data.

## Testing

The kernel logs every step with `MiniAppleKeyStore:` (never the passcode). passcoded logs to the console
with `passcoded:`; with the boot-argument `passcode_debug=1` it also asks SpringBoard (libhakios.sb.dylib's
`com.hakios.layers`) two seconds after each change of the lock state to log whether it counts the user as authenticated
("hakios: layers: the user is authenticated, a passcode, ..."). On the phone, the USB console's `sysctl kern.hakios.passcode` says the state: "passcode set,
lock state 1, not unlocked since boot, 0 failed attempts, 0 s of delay, grace 0 s, generation 3".

`qemu/run-passcode-test.sh` runs the kernel's own self-test in QEMU, on an HFS Plus volume mounted on `/private/var` as on the
phone, over three boots of the same volume: (1) set a passcode, lock, four wrong tries with no delay, a fifth with a minute's
delay, the right one refused and not counted during the delay, unlocked, a change refused with a wrong old passcode, then
accepted; (2) after the restart it is still set and locked and not "unlocked since boot", the metadata kept, a wrong try counted,
the right one unlocks (the first unlock), removed, another set; (3) with `passcode_reset=1`: none. Each boot ends with
`passcode_test: PASS`. The self-test is the sysctl `kern.hakios.passcode_selftest`, refused (EPERM) unless the boot-argument
`passcode_selftest=N` is given.
