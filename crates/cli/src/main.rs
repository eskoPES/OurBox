//! `jackbox` command-line tool.
//!
//!   jackbox inspect <file>   identify an XVD/XVC package or PE executable
//!   jackbox imports <exe>    show which imports JackBox / the host can satisfy
//!   jackbox exports <dll>    list a DLL's named exports
//!
//! Running titles comes in milestone M1 (see docs/ARCHITECTURE.md).

use jackbox_hle::{coverage, Provider};
use std::process::ExitCode;

fn usage() -> ExitCode {
    eprintln!("usage: jackbox <inspect|imports|exports> <file>");
    ExitCode::from(2)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (Some(cmd), Some(path)) = (args.first(), args.get(1)) else { return usage() };
    let data = match std::fs::read(path) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("jackbox: cannot read {path}: {e}");
            return ExitCode::FAILURE;
        }
    };
    let result = match cmd.as_str() {
        "inspect" => inspect(&data),
        "imports" => imports(&data),
        "exports" => exports(&data),
        _ => return usage(),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("jackbox: {e}");
            ExitCode::FAILURE
        }
    }
}

type Res = Result<(), Box<dyn std::error::Error>>;

fn inspect(data: &[u8]) -> Res {
    if jackbox_xvd::looks_like_xvd(data) {
        let h = jackbox_xvd::parse_header(data)?;
        println!("Format:          XVD{}", if h.is_xvc() { " (XVC game package)" } else { "" });
        println!("Content type:    {} ({:#x})", h.content_type_name(), h.content_type);
        println!("XVD type:        {:?}", h.xvd_type);
        println!("Format version:  {}", h.format_version);
        println!("Drive size:      {} MiB", h.drive_size >> 20);
        println!("Content ID:      {}", h.content_id);
        println!("Product ID:      {}", h.product_id);
        println!("Build ID:        {}", h.build_id);
        println!("Volume flags:    {:#010x}", h.volume_flags);
        if let Some(t) = h.creation_unix_secs() {
            println!("Created (unix):  {t}");
        }
        if h.is_encrypted() {
            println!("\nPayload is encrypted. JackBox cannot and will not decrypt it.");
        } else {
            println!("\nPayload is not encrypted. Mounting is planned (not in v0.1).");
        }
        return Ok(());
    }
    if jackbox_pe::looks_like_pe(data) {
        let img = jackbox_pe::parse(data)?;
        println!("Format:          PE32+ {}", if img.is_dll() { "DLL" } else { "executable" });
        println!("Machine:         {:#06x}{}", img.machine, if img.machine == jackbox_pe::MACHINE_AMD64 { " (x86-64)" } else { "" });
        println!("Image base:      {:#x}", img.image_base);
        println!("Entry point RVA: {:#x}", img.entry_point);
        println!("Subsystem:       {}", img.subsystem);
        println!("Sections:");
        for s in &img.sections {
            println!("  {:<8} va={:#010x} vsize={:#x} raw={:#x}", s.name, s.virtual_address, s.virtual_size, s.raw_size);
        }
        println!("Imports:         {} modules", img.imports.len());
        println!("Exports:         {} names", img.exports.len());
        return Ok(());
    }
    Err("not an XVD package or PE file".into())
}

fn imports(data: &[u8]) -> Res {
    let img = jackbox_pe::parse(data)?;
    let cov = coverage(&img.imports);
    for m in &cov.modules {
        let tag = match m.provider {
            Provider::Host => "host",
            Provider::JackBox => "jackbox",
            Provider::Unknown => "UNKNOWN",
        };
        println!("{:<40} [{tag}] {}/{}", m.dll, m.implemented.len(), m.implemented.len() + m.missing.len());
        if m.provider != Provider::Host {
            for s in &m.missing {
                println!("    missing  {s}");
            }
        }
    }
    let (i, t) = cov.jackbox_totals();
    println!("\nXbox-specific coverage: {i}/{t} imports implemented");
    let unknown: Vec<_> = cov.unknown_modules().map(|m| m.dll.as_str()).collect();
    if !unknown.is_empty() {
        println!("Unclassified modules (please report): {}", unknown.join(", "));
    }
    Ok(())
}

fn exports(data: &[u8]) -> Res {
    for e in jackbox_pe::parse(data)?.exports {
        println!("{e}");
    }
    Ok(())
}
