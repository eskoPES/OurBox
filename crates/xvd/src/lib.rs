//! XVD / XVC container header parser (metadata only).
//!
//! The header is unencrypted and has been documented by community research
//! (xvdtool, xboxoneresearch wiki). This crate reads it so JackBox can identify
//! a package. It does NOT and will NOT decrypt anything: content keys live in
//! console hardware and JackBox ships no keys.
//!
//! Offsets are absolute file offsets. Layout per the xboxoneresearch
//! "XVD visual format" (v0.9, derived from xvdtool). Enum numeric values marked
//! (guess) are from memory of xvdtool and must be verified against it.

use std::fmt;

pub const HEADER_START: usize = 0x200;
pub const HEADER_END: usize = 0xE00;

mod off {
    pub const MAGIC: usize = 0x200;
    pub const VOLUME_FLAGS: usize = 0x208;
    pub const FORMAT_VERSION: usize = 0x20C;
    pub const FILETIME: usize = 0x210;
    pub const DRIVE_SIZE: usize = 0x218;
    pub const CONTENT_ID: usize = 0x220;
    pub const USER_ID: usize = 0x230;
    pub const TOP_HASH: usize = 0x240;
    pub const XVD_TYPE: usize = 0x280;
    pub const CONTENT_TYPE: usize = 0x284;
    pub const EMBEDDED_XVD_LEN: usize = 0x288;
    pub const USER_DATA_LEN: usize = 0x28C;
    pub const XVC_DATA_LEN: usize = 0x290;
    pub const DYNAMIC_HEADER_LEN: usize = 0x294;
    pub const BLOCK_SIZE: usize = 0x298;
    pub const SANDBOX_ID: usize = 0x38C;
    pub const PRODUCT_ID: usize = 0x39C;
    pub const BUILD_ID: usize = 0x3AC;
    pub const SEQUENCE_NUMBER: usize = 0x48C;
    pub const REQUIRED_SYSTEM_VERSION: usize = 0x494;
    pub const ODK_INDEX: usize = 0x49C;
}

#[derive(Debug, PartialEq, Eq)]
pub enum XvdError {
    TooShort,
    BadMagic,
}
impl fmt::Display for XvdError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            XvdError::TooShort => write!(f, "file shorter than an XVD header (0xE00 bytes)"),
            XvdError::BadMagic => write!(f, "no msft-xvd magic at 0x200"),
        }
    }
}
impl std::error::Error for XvdError {}

/// Volume flag bits. (guess) values from xvdtool, verify.
pub mod flags {
    pub const READ_ONLY: u32 = 0x1;
    pub const ENCRYPTION_DISABLED: u32 = 0x2;
    pub const DATA_INTEGRITY_DISABLED: u32 = 0x4;
    pub const LEGACY_SECTOR_SIZE: u32 = 0x8;
    pub const RESILIENCY_ENABLED: u32 = 0x10;
    pub const SRA_READ_ONLY: u32 = 0x20;
    pub const REGION_ID_IN_XTS: u32 = 0x40;
    pub const ERA_SPECIFIC: u32 = 0x80;
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum XvdType {
    Fixed,
    Dynamic,
    Other(u32),
}

/// Content types. (guess) numbering from xvdtool, verify.
pub fn content_type_name(v: u32) -> &'static str {
    const NAMES: &[&str] = &[
        "Data", "Title", "SystemOS", "EraOS", "Scratch", "ResetData", "Application", "HostOS",
        "X360STFS", "X360FATX", "X360GDFX", "Updater", "OfflineUpdater", "Template", "MteHost",
        "MteApp", "MteTitle", "MteEraOS", "EraTools", "SystemTools", "SystemAux", "AcousticModel",
        "SystemCodecsVolume", "QasltPackage", "AppDlc", "TitleDlc", "UniversalDlc",
        "SystemDataVolume", "TestVolume", "HardwareTestVolume", "KioskContent", "Unknown1F",
        "HostProfiler", "Uwa",
    ];
    NAMES.get(v as usize).copied().unwrap_or("Unknown")
}

#[derive(Debug, Clone)]
pub struct XvdHeader {
    pub volume_flags: u32,
    pub format_version: u32,
    /// Windows FILETIME (100 ns ticks since 1601-01-01).
    pub creation_filetime: u64,
    pub drive_size: u64,
    pub content_id: Guid,
    pub user_id: Guid,
    pub top_hash: [u8; 32],
    pub xvd_type: XvdType,
    pub content_type: u32,
    pub embedded_xvd_len: u32,
    pub user_data_len: u32,
    pub xvc_data_len: u32,
    pub dynamic_header_len: u32,
    pub block_size: u32,
    pub sandbox_id: [u8; 16],
    pub product_id: Guid,
    pub build_id: Guid,
    pub sequence_number: u64,
    pub required_system_version: u64,
    pub odk_index: u32,
}

impl XvdHeader {
    pub fn is_xvc(&self) -> bool {
        self.xvc_data_len != 0
    }
    /// True when the header says the payload is encrypted. JackBox can only
    /// mount payloads where this is false.
    pub fn is_encrypted(&self) -> bool {
        self.volume_flags & flags::ENCRYPTION_DISABLED == 0
    }
    pub fn content_type_name(&self) -> &'static str {
        content_type_name(self.content_type)
    }
    /// Creation time as Unix seconds, if representable.
    pub fn creation_unix_secs(&self) -> Option<i64> {
        const EPOCH_DIFF: i64 = 11_644_473_600;
        let secs = (self.creation_filetime / 10_000_000) as i64 - EPOCH_DIFF;
        (secs > 0).then_some(secs)
    }
}

/// A GUID stored in Windows mixed-endian layout.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Guid(pub [u8; 16]);
impl fmt::Display for Guid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let b = &self.0;
        write!(
            f,
            "{:08x}-{:04x}-{:04x}-{:02x}{:02x}-{:02x}{:02x}{:02x}{:02x}{:02x}{:02x}",
            u32::from_le_bytes([b[0], b[1], b[2], b[3]]),
            u16::from_le_bytes([b[4], b[5]]),
            u16::from_le_bytes([b[6], b[7]]),
            b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]
        )
    }
}
impl fmt::Debug for Guid {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{self}")
    }
}

pub fn looks_like_xvd(b: &[u8]) -> bool {
    b.get(off::MAGIC..off::MAGIC + 8).is_some_and(|m| m.eq_ignore_ascii_case(b"msft-xvd"))
}

pub fn parse_header(b: &[u8]) -> Result<XvdHeader, XvdError> {
    if b.len() < HEADER_END {
        return Err(XvdError::TooShort);
    }
    if !looks_like_xvd(b) {
        return Err(XvdError::BadMagic);
    }
    let u32_ = |o: usize| u32::from_le_bytes(b[o..o + 4].try_into().unwrap());
    let u64_ = |o: usize| u64::from_le_bytes(b[o..o + 8].try_into().unwrap());
    let b16 = |o: usize| -> [u8; 16] { b[o..o + 16].try_into().unwrap() };
    Ok(XvdHeader {
        volume_flags: u32_(off::VOLUME_FLAGS),
        format_version: u32_(off::FORMAT_VERSION),
        creation_filetime: u64_(off::FILETIME),
        drive_size: u64_(off::DRIVE_SIZE),
        content_id: Guid(b16(off::CONTENT_ID)),
        user_id: Guid(b16(off::USER_ID)),
        top_hash: b[off::TOP_HASH..off::TOP_HASH + 32].try_into().unwrap(),
        xvd_type: match u32_(off::XVD_TYPE) {
            0 => XvdType::Fixed, // (guess)
            1 => XvdType::Dynamic, // (guess)
            v => XvdType::Other(v),
        },
        content_type: u32_(off::CONTENT_TYPE),
        embedded_xvd_len: u32_(off::EMBEDDED_XVD_LEN),
        user_data_len: u32_(off::USER_DATA_LEN),
        xvc_data_len: u32_(off::XVC_DATA_LEN),
        dynamic_header_len: u32_(off::DYNAMIC_HEADER_LEN),
        block_size: u32_(off::BLOCK_SIZE),
        sandbox_id: b16(off::SANDBOX_ID),
        product_id: Guid(b16(off::PRODUCT_ID)),
        build_id: Guid(b16(off::BUILD_ID)),
        sequence_number: u64_(off::SEQUENCE_NUMBER),
        required_system_version: u64_(off::REQUIRED_SYSTEM_VERSION),
        odk_index: u32_(off::ODK_INDEX),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn synthetic() -> Vec<u8> {
        let mut b = vec![0u8; HEADER_END];
        b[off::MAGIC..off::MAGIC + 8].copy_from_slice(b"msft-xvd");
        b[off::VOLUME_FLAGS..off::VOLUME_FLAGS + 4].copy_from_slice(&flags::ENCRYPTION_DISABLED.to_le_bytes());
        b[off::FORMAT_VERSION..off::FORMAT_VERSION + 4].copy_from_slice(&3u32.to_le_bytes());
        b[off::DRIVE_SIZE..off::DRIVE_SIZE + 8].copy_from_slice(&(64u64 << 20).to_le_bytes());
        b[off::XVD_TYPE..off::XVD_TYPE + 4].copy_from_slice(&1u32.to_le_bytes());
        b[off::CONTENT_TYPE..off::CONTENT_TYPE + 4].copy_from_slice(&0x1Cu32.to_le_bytes());
        b[off::BLOCK_SIZE..off::BLOCK_SIZE + 4].copy_from_slice(&0xAA000u32.to_le_bytes());
        b
    }

    #[test]
    fn parses_synthetic_header() {
        let h = parse_header(&synthetic()).unwrap();
        assert_eq!(h.format_version, 3);
        assert_eq!(h.drive_size, 64 << 20);
        assert_eq!(h.xvd_type, XvdType::Dynamic);
        assert_eq!(h.content_type_name(), "TestVolume");
        assert!(!h.is_encrypted());
        assert!(!h.is_xvc());
    }

    #[test]
    fn rejects_bad_magic_and_short_input() {
        assert_eq!(parse_header(&[0u8; 0x100]).unwrap_err(), XvdError::TooShort);
        assert_eq!(parse_header(&[0u8; HEADER_END]).unwrap_err(), XvdError::BadMagic);
    }

    #[test]
    fn guid_formats_mixed_endian() {
        let g = Guid([0x78, 0x56, 0x34, 0x12, 0x34, 0x12, 0x78, 0x56, 1, 2, 3, 4, 5, 6, 7, 8]);
        assert_eq!(g.to_string(), "12345678-1234-5678-0102-030405060708");
    }
}
