//! Catalog of modules a GDK / Xbox Series title may import, who provides each
//! one, and which exports JackBox's clean-room runtime implements.
//!
//! This table is the source of truth for the `jackbox imports` coverage report.
//! Module names marked (guess) come from public GDK documentation and header
//! names, not from inspecting a console binary, and must be verified.

use jackbox_pe::{ImportSymbol, ImportedModule};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Provider {
    /// Standard Win32 / CRT surface. Wine (Linux) or Windows itself supplies it.
    Host,
    /// Xbox-specific surface that JackBox must reimplement in `runtime/`.
    JackBox,
    /// Not in the catalog yet. Needs research.
    Unknown,
}

pub struct ModuleInfo {
    pub name: &'static str,
    pub provider: Provider,
    pub note: &'static str,
    /// Exports JackBox implements (only meaningful for Provider::JackBox).
    pub implemented: &'static [&'static str],
}

/// Exports implemented by runtime/xgameruntime (see xgameruntime.def). Keep in
/// sync; crates/cli/tests/runtime.rs checks the built DLL against this list.
pub const XGAMERUNTIME_IMPLEMENTED: &[&str] = &[
    "XGameRuntimeInitialize",
    "XGameRuntimeUninitialize",
    "XGameGetXboxTitleId",
    "XSystemGetDeviceType",
    "XSystemGetConsoleId",
    "XTaskQueueCreate",
    "XTaskQueueDuplicateHandle",
    "XTaskQueueCloseHandle",
    "XTaskQueueSubmitCallback",
    "XTaskQueueSubmitDelayedCallback",
    "XTaskQueueDispatch",
    "XTaskQueueIsEmpty",
    "XTaskQueueTerminate",
    "XTaskQueueGetCurrentProcessTaskQueue",
    "XTaskQueueSetCurrentProcessTaskQueue",
    "XAsyncBegin",
    "XAsyncSchedule",
    "XAsyncComplete",
    "XAsyncGetStatus",
    "XAsyncGetResultSize",
    "XAsyncGetResult",
    "XAsyncCancel",
    "XAsyncRun",
];

pub const MODULES: &[ModuleInfo] = &[
    ModuleInfo { name: "xgameruntime.dll", provider: Provider::JackBox, note: "XGame*, XSystem*, XTaskQueue, XAsync, XUser (guess: module name)", implemented: XGAMERUNTIME_IMPLEMENTED },
    ModuleInfo { name: "d3d12_x.dll", provider: Provider::JackBox, note: "D3D12.X for Xbox One family (guess: module name)", implemented: &[] },
    ModuleInfo { name: "d3d12_xs.dll", provider: Provider::JackBox, note: "D3D12.X for Series X|S (guess: module name)", implemented: &[] },
    ModuleInfo { name: "xg_x.dll", provider: Provider::JackBox, note: "XG texture/tiling helpers (guess)", implemented: &[] },
    ModuleInfo { name: "xg_xs.dll", provider: Provider::JackBox, note: "XG texture/tiling helpers, Series (guess)", implemented: &[] },
    ModuleInfo { name: "gameinput.dll", provider: Provider::JackBox, note: "GameInput controller API", implemented: &[] },
    ModuleInfo { name: "xaudio2_9.dll", provider: Provider::Host, note: "Wine provides via FAudio", implemented: &[] },
    ModuleInfo { name: "xinput1_4.dll", provider: Provider::Host, note: "Wine provides", implemented: &[] },
    ModuleInfo { name: "d3d12.dll", provider: Provider::Host, note: "vkd3d-proton on Linux", implemented: &[] },
    ModuleInfo { name: "dxgi.dll", provider: Provider::Host, note: "DXVK/vkd3d-proton on Linux", implemented: &[] },
];

const HOST_PREFIXES: &[&str] = &["api-ms-win-", "ext-ms-win-", "vcruntime", "msvcp", "ucrtbase"];
const HOST_DLLS: &[&str] = &[
    "kernel32.dll", "kernelbase.dll", "ntdll.dll", "user32.dll", "gdi32.dll", "advapi32.dll",
    "ole32.dll", "oleaut32.dll", "shell32.dll", "shlwapi.dll", "ws2_32.dll", "winhttp.dll",
    "bcrypt.dll", "crypt32.dll", "dbghelp.dll", "version.dll", "winmm.dll", "combase.dll",
];

pub fn lookup(dll: &str) -> Option<&'static ModuleInfo> {
    MODULES.iter().find(|m| m.name.eq_ignore_ascii_case(dll))
}

pub fn classify(dll: &str) -> Provider {
    if let Some(m) = lookup(dll) {
        return m.provider;
    }
    let l = dll.to_ascii_lowercase();
    if HOST_DLLS.contains(&l.as_str()) || HOST_PREFIXES.iter().any(|p| l.starts_with(p)) {
        Provider::Host
    } else {
        Provider::Unknown
    }
}

#[derive(Debug)]
pub struct ModuleCoverage {
    pub dll: String,
    pub provider: Provider,
    pub implemented: Vec<String>,
    pub missing: Vec<String>,
}

#[derive(Debug, Default)]
pub struct Coverage {
    pub modules: Vec<ModuleCoverage>,
}

impl Coverage {
    /// (implemented, total) across JackBox-provided modules only.
    pub fn jackbox_totals(&self) -> (usize, usize) {
        self.modules
            .iter()
            .filter(|m| m.provider == Provider::JackBox)
            .fold((0, 0), |(i, t), m| (i + m.implemented.len(), t + m.implemented.len() + m.missing.len()))
    }
    pub fn unknown_modules(&self) -> impl Iterator<Item = &ModuleCoverage> {
        self.modules.iter().filter(|m| m.provider == Provider::Unknown)
    }
}

pub fn coverage(imports: &[ImportedModule]) -> Coverage {
    let mut cov = Coverage::default();
    for m in imports {
        let provider = classify(&m.dll);
        let impls = lookup(&m.dll).map(|i| i.implemented).unwrap_or(&[]);
        let (mut implemented, mut missing) = (Vec::new(), Vec::new());
        for s in &m.symbols {
            let name = s.to_string();
            let done = provider == Provider::Host
                || matches!(s, ImportSymbol::Name { name, .. } if impls.contains(&name.as_str()));
            if done { implemented.push(name) } else { missing.push(name) }
        }
        cov.modules.push(ModuleCoverage { dll: m.dll.clone(), provider, implemented, missing });
    }
    cov
}

#[cfg(test)]
mod tests {
    use super::*;

    fn named(n: &str) -> ImportSymbol {
        ImportSymbol::Name { hint: 0, name: n.into() }
    }

    #[test]
    fn classifies_modules() {
        assert_eq!(classify("KERNEL32.dll"), Provider::Host);
        assert_eq!(classify("api-ms-win-core-synch-l1-2-0.dll"), Provider::Host);
        assert_eq!(classify("XGameRuntime.dll"), Provider::JackBox);
        assert_eq!(classify("mystery.dll"), Provider::Unknown);
    }

    #[test]
    fn computes_coverage() {
        let imports = vec![
            ImportedModule { dll: "kernel32.dll".into(), symbols: vec![named("Sleep")] },
            ImportedModule {
                dll: "xgameruntime.dll".into(),
                symbols: vec![named("XGameRuntimeInitialize"), named("XUserAddAsync")],
            },
        ];
        let c = coverage(&imports);
        assert_eq!(c.jackbox_totals(), (1, 2));
        assert_eq!(c.modules[1].missing, vec!["XUserAddAsync".to_string()]);
    }
}
