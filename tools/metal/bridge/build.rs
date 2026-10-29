// SPDX-License-Identifier: MIT
// Copyright (c) 2026 the HakiOS authors (Scion).
// What the bridge's sources take from the build (tools/metal/mk-metal2vulkan.sh sets it):
//   M2V_FIXTURE_AIR  the self-test's library as AIR bitcode (tools/metal/m2v_selftest.metal), embedded; empty when not given
//   M2V_FC_AIR       the function-constant kernel (tools/metal/m2v_constant.metal), the same way
//   M2V_RUNTIME_ID   the runtime's pin and patches, which key the shader cache (nmb_runtime_build)
//   M2V_INSTALL_NAME the dylib's install name (/redwood/lib/libmetal2vulkan.dylib for iOS)
use std::path::PathBuf;

fn embedded(var: &str, out: &str) {
    println!("cargo:rerun-if-env-changed={var}");
    let path = match std::env::var_os(var) {
        Some(path) if !path.is_empty() => PathBuf::from(path),
        _ => {
            let empty = PathBuf::from(std::env::var_os("OUT_DIR").unwrap()).join(out);
            std::fs::write(&empty, b"").unwrap();
            empty
        }
    };
    println!("cargo:rerun-if-changed={}", path.display());
    println!("cargo:rustc-env={out}={}", path.display());
}

fn main() {
    embedded("M2V_FIXTURE_AIR", "NMB_FIXTURE_AIR");
    embedded("M2V_FC_AIR", "NMB_FC_AIR");
    println!("cargo:rerun-if-env-changed=M2V_RUNTIME_ID");
    println!("cargo:rustc-env=NMB_RUNTIME_ID={}", std::env::var("M2V_RUNTIME_ID").unwrap_or_else(|_| "unknown".into()));
    println!("cargo:rerun-if-env-changed=M2V_INSTALL_NAME");
    if let Ok(name) = std::env::var("M2V_INSTALL_NAME") {
        println!("cargo:rustc-cdylib-link-arg=-Wl,-install_name,{name}");
    }
}
