//! Checks the MinGW-built runtime (make) against the HLE catalog, and the
//! coverage report against a test title. Skips if `make` has not been run.

use std::path::PathBuf;

fn out() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../build/win64")
}

#[test]
fn dll_exports_match_catalog() {
    let Ok(bytes) = std::fs::read(out().join("xgameruntime.dll")) else {
        eprintln!("skipping: run `make` first");
        return;
    };
    let dll = jackbox_pe::parse(&bytes).unwrap();
    let mut exported = dll.exports.clone();
    let mut catalog: Vec<_> = jackbox_hle::XGAMERUNTIME_IMPLEMENTED.iter().map(|s| s.to_string()).collect();
    exported.sort();
    catalog.sort();
    assert_eq!(exported, catalog, "runtime exports and hle catalog are out of sync");
}

#[test]
fn coverage_flags_unimplemented_title_import() {
    let Ok(bytes) = std::fs::read(out().join("titles/t_unimpl.exe")) else { return };
    let exe = jackbox_pe::parse(&bytes).unwrap();
    let cov = jackbox_hle::coverage(&exe.imports);
    let xgr = cov.modules.iter().find(|m| m.dll.eq_ignore_ascii_case("xgameruntime.dll")).unwrap();
    assert_eq!(xgr.missing, vec!["XPackageGetCurrentProcessPackageIdentifier".to_string()]);
}
