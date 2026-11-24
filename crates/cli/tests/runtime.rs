//! Builds the runtime with runtime/build.sh (needs clang + lld-link) and checks
//! that the real DLL and the HLE catalog agree. Skips if the toolchain is absent.

use std::path::PathBuf;
use std::process::Command;

fn root() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn build_runtime() -> Option<PathBuf> {
    let ok = Command::new("bash").arg(root().join("runtime/build.sh")).status().ok()?.success();
    ok.then(|| root().join("runtime/build"))
}

#[test]
fn dll_exports_match_catalog_and_sample_coverage() {
    let Some(out) = build_runtime() else {
        eprintln!("skipping: clang/lld-link not available");
        return;
    };
    let dll = jackbox_pe::parse(&std::fs::read(out.join("xgameruntime.dll")).unwrap()).unwrap();
    let mut exported: Vec<_> = dll.exports.iter().filter(|e| !e.starts_with("JackBox")).cloned().collect();
    let mut catalog: Vec<_> = jackbox_hle::XGAMERUNTIME_IMPLEMENTED.iter().map(|s| s.to_string()).collect();
    exported.sort();
    catalog.sort();
    assert_eq!(exported, catalog, "runtime exports and hle catalog are out of sync");

    let exe = jackbox_pe::parse(&std::fs::read(out.join("hello.exe")).unwrap()).unwrap();
    let cov = jackbox_hle::coverage(&exe.imports);
    assert_eq!(cov.jackbox_totals(), (3, 4));
}
