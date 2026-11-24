//! Minimal, dependency-free PE32+ (x86-64) parser.
//!
//! Enough to identify a GDK-style executable and walk its import table so the
//! loader and the coverage report know which modules a title needs.
//! Delay-load imports and exports are TODO.

use std::fmt;

#[derive(Debug, PartialEq, Eq)]
pub enum PeError {
    TooShort,
    NoMzSignature,
    NoPeSignature,
    NotPe32Plus(u16),
    BadRva(u32),
    BadString,
}

impl fmt::Display for PeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            PeError::TooShort => write!(f, "file is truncated"),
            PeError::NoMzSignature => write!(f, "missing MZ signature"),
            PeError::NoPeSignature => write!(f, "missing PE\\0\\0 signature"),
            PeError::NotPe32Plus(m) => write!(f, "optional header magic {m:#x} is not PE32+ (0x20b)"),
            PeError::BadRva(r) => write!(f, "RVA {r:#x} does not map to any section"),
            PeError::BadString => write!(f, "unterminated or non-UTF-8 string"),
        }
    }
}
impl std::error::Error for PeError {}

pub const MACHINE_AMD64: u16 = 0x8664;
pub const DIR_EXPORT: usize = 0;
pub const DIR_IMPORT: usize = 1;
pub const DIR_DELAY_IMPORT: usize = 13;

#[derive(Debug, Clone)]
pub struct Section {
    pub name: String,
    pub virtual_size: u32,
    pub virtual_address: u32,
    pub raw_size: u32,
    pub raw_offset: u32,
    pub characteristics: u32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ImportSymbol {
    Name { hint: u16, name: String },
    Ordinal(u16),
}

impl fmt::Display for ImportSymbol {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            ImportSymbol::Name { name, .. } => write!(f, "{name}"),
            ImportSymbol::Ordinal(o) => write!(f, "#{o}"),
        }
    }
}

#[derive(Debug, Clone)]
pub struct ImportedModule {
    pub dll: String,
    pub symbols: Vec<ImportSymbol>,
}

#[derive(Debug, Clone)]
pub struct PeImage {
    pub machine: u16,
    pub characteristics: u16,
    pub image_base: u64,
    pub entry_point: u32,
    pub size_of_image: u32,
    pub subsystem: u16,
    pub sections: Vec<Section>,
    pub data_dirs: Vec<(u32, u32)>,
    pub imports: Vec<ImportedModule>,
    /// Named exports (DLLs). Forwarders and ordinal-only exports are skipped.
    pub exports: Vec<String>,
}

impl PeImage {
    pub fn is_dll(&self) -> bool {
        self.characteristics & 0x2000 != 0
    }
}

fn u16_at(b: &[u8], o: usize) -> Result<u16, PeError> {
    b.get(o..o + 2).map(|s| u16::from_le_bytes([s[0], s[1]])).ok_or(PeError::TooShort)
}
fn u32_at(b: &[u8], o: usize) -> Result<u32, PeError> {
    b.get(o..o + 4).map(|s| u32::from_le_bytes(s.try_into().unwrap())).ok_or(PeError::TooShort)
}
fn u64_at(b: &[u8], o: usize) -> Result<u64, PeError> {
    b.get(o..o + 8).map(|s| u64::from_le_bytes(s.try_into().unwrap())).ok_or(PeError::TooShort)
}

/// Quick check used by format auto-detection.
pub fn looks_like_pe(b: &[u8]) -> bool {
    b.starts_with(b"MZ")
}

pub fn parse(b: &[u8]) -> Result<PeImage, PeError> {
    if b.len() < 0x40 {
        return Err(PeError::TooShort);
    }
    if !looks_like_pe(b) {
        return Err(PeError::NoMzSignature);
    }
    let nt = u32_at(b, 0x3C)? as usize;
    if b.get(nt..nt + 4) != Some(b"PE\0\0") {
        return Err(PeError::NoPeSignature);
    }
    let coff = nt + 4;
    let machine = u16_at(b, coff)?;
    let nsect = u16_at(b, coff + 2)? as usize;
    let opt_size = u16_at(b, coff + 16)? as usize;
    let characteristics = u16_at(b, coff + 18)?;
    let opt = coff + 20;
    let magic = u16_at(b, opt)?;
    if magic != 0x20B {
        return Err(PeError::NotPe32Plus(magic));
    }
    let entry_point = u32_at(b, opt + 16)?;
    let image_base = u64_at(b, opt + 24)?;
    let size_of_image = u32_at(b, opt + 56)?;
    let subsystem = u16_at(b, opt + 68)?;
    let ndirs = (u32_at(b, opt + 108)? as usize).min(16);
    let mut data_dirs = Vec::with_capacity(ndirs);
    for i in 0..ndirs {
        data_dirs.push((u32_at(b, opt + 112 + i * 8)?, u32_at(b, opt + 116 + i * 8)?));
    }

    let sh = opt + opt_size;
    let mut sections = Vec::with_capacity(nsect);
    for i in 0..nsect {
        let s = sh + i * 40;
        let raw_name = b.get(s..s + 8).ok_or(PeError::TooShort)?;
        let end = raw_name.iter().position(|&c| c == 0).unwrap_or(8);
        sections.push(Section {
            name: String::from_utf8_lossy(&raw_name[..end]).into_owned(),
            virtual_size: u32_at(b, s + 8)?,
            virtual_address: u32_at(b, s + 12)?,
            raw_size: u32_at(b, s + 16)?,
            raw_offset: u32_at(b, s + 20)?,
            characteristics: u32_at(b, s + 36)?,
        });
    }

    let mut img = PeImage {
        machine,
        characteristics,
        image_base,
        entry_point,
        size_of_image,
        subsystem,
        sections,
        data_dirs,
        imports: Vec::new(),
        exports: Vec::new(),
    };
    img.imports = parse_imports(b, &img)?;
    img.exports = parse_exports(b, &img)?;
    Ok(img)
}

/// Translate an RVA into a file offset using the section table.
pub fn rva_to_offset(img: &PeImage, rva: u32) -> Result<usize, PeError> {
    for s in &img.sections {
        let span = s.virtual_size.max(s.raw_size);
        if rva >= s.virtual_address && rva < s.virtual_address + span {
            let delta = rva - s.virtual_address;
            if delta >= s.raw_size {
                return Err(PeError::BadRva(rva)); // in .bss-like tail, not in file
            }
            return Ok((s.raw_offset + delta) as usize);
        }
    }
    Err(PeError::BadRva(rva))
}

fn cstr_at(b: &[u8], o: usize) -> Result<String, PeError> {
    let tail = b.get(o..).ok_or(PeError::TooShort)?;
    let end = tail.iter().position(|&c| c == 0).ok_or(PeError::BadString)?;
    std::str::from_utf8(&tail[..end]).map(str::to_owned).map_err(|_| PeError::BadString)
}

fn parse_exports(b: &[u8], img: &PeImage) -> Result<Vec<String>, PeError> {
    let Some(&(rva, size)) = img.data_dirs.get(DIR_EXPORT) else { return Ok(vec![]) };
    if rva == 0 || size == 0 {
        return Ok(vec![]);
    }
    let d = rva_to_offset(img, rva)?;
    let nnames = u32_at(b, d + 24)? as usize;
    let names = rva_to_offset(img, u32_at(b, d + 32)?)?;
    (0..nnames)
        .map(|i| cstr_at(b, rva_to_offset(img, u32_at(b, names + i * 4)?)?))
        .collect()
}

fn parse_imports(b: &[u8], img: &PeImage) -> Result<Vec<ImportedModule>, PeError> {
    let Some(&(rva, size)) = img.data_dirs.get(DIR_IMPORT) else { return Ok(vec![]) };
    if rva == 0 || size == 0 {
        return Ok(vec![]);
    }
    let mut out = Vec::new();
    let mut d = rva_to_offset(img, rva)?;
    loop {
        let oft = u32_at(b, d)?;
        let name_rva = u32_at(b, d + 12)?;
        let ft = u32_at(b, d + 16)?;
        if oft == 0 && name_rva == 0 && ft == 0 {
            break;
        }
        let dll = cstr_at(b, rva_to_offset(img, name_rva)?)?;
        let thunk_rva = if oft != 0 { oft } else { ft };
        let mut t = rva_to_offset(img, thunk_rva)?;
        let mut symbols = Vec::new();
        loop {
            let v = u64_at(b, t)?;
            if v == 0 {
                break;
            }
            if v & (1 << 63) != 0 {
                symbols.push(ImportSymbol::Ordinal((v & 0xFFFF) as u16));
            } else {
                let hn = rva_to_offset(img, (v & 0x7FFF_FFFF) as u32)?;
                symbols.push(ImportSymbol::Name { hint: u16_at(b, hn)?, name: cstr_at(b, hn + 2)? });
            }
            t += 8;
        }
        out.push(ImportedModule { dll, symbols });
        d += 20;
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Builds a tiny PE32+ with one `.idata` section importing from two DLLs.
    pub fn build_test_pe(imports: &[(&str, &[&str])]) -> Vec<u8> {
        const SECT_RVA: u32 = 0x1000;
        const SECT_OFF: u32 = 0x200;
        // Lay out .idata: descriptors, then per-DLL thunk arrays, then names.
        let ndesc = imports.len() + 1;
        let mut data = vec![0u8; ndesc * 20];
        let mut descs = Vec::new();
        for (dll, funcs) in imports {
            let thunk_off = data.len();
            data.resize(data.len() + (funcs.len() + 1) * 8, 0);
            let mut thunks = Vec::new();
            for f in *funcs {
                let hn = data.len() as u32 + SECT_RVA;
                data.extend_from_slice(&0u16.to_le_bytes());
                data.extend_from_slice(f.as_bytes());
                data.push(0);
                if data.len() % 2 == 1 {
                    data.push(0);
                }
                thunks.push(hn as u64);
            }
            for (i, t) in thunks.iter().enumerate() {
                data[thunk_off + i * 8..thunk_off + i * 8 + 8].copy_from_slice(&t.to_le_bytes());
            }
            let name_rva = data.len() as u32 + SECT_RVA;
            data.extend_from_slice(dll.as_bytes());
            data.push(0);
            descs.push((thunk_off as u32 + SECT_RVA, name_rva));
        }
        for (i, (thunk, name)) in descs.iter().enumerate() {
            let o = i * 20;
            data[o..o + 4].copy_from_slice(&thunk.to_le_bytes());
            data[o + 12..o + 16].copy_from_slice(&name.to_le_bytes());
            data[o + 16..o + 20].copy_from_slice(&thunk.to_le_bytes());
        }

        let mut f = vec![0u8; SECT_OFF as usize];
        f[0..2].copy_from_slice(b"MZ");
        f[0x3C..0x40].copy_from_slice(&0x80u32.to_le_bytes());
        let nt = 0x80;
        f[nt..nt + 4].copy_from_slice(b"PE\0\0");
        let coff = nt + 4;
        f[coff..coff + 2].copy_from_slice(&MACHINE_AMD64.to_le_bytes());
        f[coff + 2..coff + 4].copy_from_slice(&1u16.to_le_bytes());
        f[coff + 16..coff + 18].copy_from_slice(&240u16.to_le_bytes());
        f[coff + 18..coff + 20].copy_from_slice(&0x22u16.to_le_bytes());
        let opt = coff + 20;
        f[opt..opt + 2].copy_from_slice(&0x20Bu16.to_le_bytes());
        f[opt + 24..opt + 32].copy_from_slice(&0x1_4000_0000u64.to_le_bytes());
        f[opt + 68..opt + 70].copy_from_slice(&2u16.to_le_bytes());
        f[opt + 108..opt + 112].copy_from_slice(&16u32.to_le_bytes());
        let idir = opt + 112 + DIR_IMPORT * 8;
        f[idir..idir + 4].copy_from_slice(&SECT_RVA.to_le_bytes());
        f[idir + 4..idir + 8].copy_from_slice(&((ndesc * 20) as u32).to_le_bytes());
        let sh = opt + 240;
        f[sh..sh + 6].copy_from_slice(b".idata");
        f[sh + 8..sh + 12].copy_from_slice(&(data.len() as u32).to_le_bytes());
        f[sh + 12..sh + 16].copy_from_slice(&SECT_RVA.to_le_bytes());
        f[sh + 16..sh + 20].copy_from_slice(&(data.len() as u32).to_le_bytes());
        f[sh + 20..sh + 24].copy_from_slice(&SECT_OFF.to_le_bytes());
        f.extend_from_slice(&data);
        f
    }

    #[test]
    fn parses_synthetic_imports() {
        let pe = build_test_pe(&[
            ("KERNEL32.dll", &["CreateFileW", "ExitProcess"]),
            ("xgameruntime.dll", &["XGameRuntimeInitialize"]),
        ]);
        let img = parse(&pe).unwrap();
        assert_eq!(img.machine, MACHINE_AMD64);
        assert_eq!(img.image_base, 0x1_4000_0000);
        assert_eq!(img.sections[0].name, ".idata");
        assert_eq!(img.imports.len(), 2);
        assert_eq!(img.imports[1].dll, "xgameruntime.dll");
        assert_eq!(img.imports[0].symbols[1].to_string(), "ExitProcess");
    }

    #[test]
    fn rejects_garbage() {
        assert_eq!(parse(&[0u8; 0x100]).unwrap_err(), PeError::NoMzSignature);
        assert_eq!(parse(b"MZ").unwrap_err(), PeError::TooShort);
    }
}
