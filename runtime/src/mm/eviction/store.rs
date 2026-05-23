//! RAM-backed staging store for runtime eviction blobs.
//!
//! This layer sits between blob parsing and policy integration:
//! callers can stage a validated blob, atomically promote it to the
//! active slot for that kind, roll back to the prior active blob, and
//! inspect metadata for shell/UI status reporting.

use core::ptr::addr_of_mut;
use core::sync::atomic::{AtomicBool, Ordering};

use super::blob::{parse_blob, BlobError, BlobHeader, BlobKind, ParsedBlob};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum SlotState {
    Empty = 0,
    Staged = 1,
    Active = 2,
    RolledBack = 3,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StoreError {
    Parse(BlobError),
    NoStagedBlob,
    NoRollbackBlob,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BlobMetadata {
    pub version: u16,
    pub kind: BlobKind,
    pub feature_schema_version: u16,
    pub payload_len: u32,
    pub checksum: u32,
}

impl From<BlobHeader> for BlobMetadata {
    fn from(header: BlobHeader) -> Self {
        Self {
            version: header.version,
            kind: header.kind,
            feature_schema_version: header.feature_schema_version,
            payload_len: header.payload_len,
            checksum: header.checksum,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BlobStatus {
    pub kind: BlobKind,
    pub state: SlotState,
    pub staged: Option<BlobMetadata>,
    pub active: Option<BlobMetadata>,
    pub rollback: Option<BlobMetadata>,
}

#[derive(Clone)]
struct KindStore {
    staged: Option<ParsedBlob>,
    active: Option<ParsedBlob>,
    rollback: Option<ParsedBlob>,
    state: SlotState,
}

impl KindStore {
    const fn new() -> Self {
        Self {
            staged: None,
            active: None,
            rollback: None,
            state: SlotState::Empty,
        }
    }

    fn status(&self, kind: BlobKind) -> BlobStatus {
        BlobStatus {
            kind,
            state: self.state,
            staged: self.staged.as_ref().map(|blob| blob.header.into()),
            active: self.active.as_ref().map(|blob| blob.header.into()),
            rollback: self.rollback.as_ref().map(|blob| blob.header.into()),
        }
    }
}

static STORE_LOCK: AtomicBool = AtomicBool::new(false);
static mut STORES: [KindStore; 3] = [
    KindStore::new(),
    KindStore::new(),
    KindStore::new(),
];

struct SpinGuard;

impl SpinGuard {
    fn new() -> Self {
        while STORE_LOCK
            .compare_exchange_weak(false, true, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            core::hint::spin_loop();
        }
        SpinGuard
    }
}

impl Drop for SpinGuard {
    fn drop(&mut self) {
        STORE_LOCK.store(false, Ordering::Release);
    }
}

const fn kind_index(kind: BlobKind) -> usize {
    match kind {
        BlobKind::XGBoost => 0,
        BlobKind::Mlp => 1,
        BlobKind::CacheusConfig => 2,
    }
}

// SAFETY: Marked `unsafe fn` so callers cannot accidentally invoke
// these without acknowledging the lock requirement. They return raw
// pointers; dereferencing requires the SpinGuard to be held. The
// pointer arithmetic uses `addr_of_mut!` cast to *mut/*const without
// ever materializing an intermediate reference, which is what the
// previous implementation accidentally did via `&mut ... as *mut`.
unsafe fn store_mut(kind: BlobKind) -> *mut KindStore {
    let base = addr_of_mut!(STORES) as *mut KindStore;
    base.add(kind_index(kind))
}

unsafe fn store_ref(kind: BlobKind) -> *const KindStore {
    let base = addr_of_mut!(STORES) as *const KindStore;
    base.add(kind_index(kind))
}

pub fn reset() {
    let _g = SpinGuard::new();
    unsafe {
        let stores = addr_of_mut!(STORES);
        (*stores)[0] = KindStore::new();
        (*stores)[1] = KindStore::new();
        (*stores)[2] = KindStore::new();
    }
}

pub fn stage_blob(bytes: &[u8]) -> Result<BlobKind, StoreError> {
    let parsed = parse_blob(bytes).map_err(StoreError::Parse)?;
    stage_parsed(parsed)
}

pub fn stage_parsed(parsed: ParsedBlob) -> Result<BlobKind, StoreError> {
    let kind = parsed.header.kind;
    let _g = SpinGuard::new();
    unsafe {
        let store = &mut *store_mut(kind);
        store.staged = Some(parsed);
        store.state = SlotState::Staged;
    }
    Ok(kind)
}

pub fn activate(kind: BlobKind) -> Result<(), StoreError> {
    {
        let _g = SpinGuard::new();
        // SAFETY: _g held — exclusive access to this kind's store slot.
        unsafe {
            let store = &mut *store_mut(kind);
            let staged = store.staged.take().ok_or(StoreError::NoStagedBlob)?;
            store.rollback = store.active.take();
            store.active = Some(staged);
            store.state = SlotState::Active;
        }
    }

    // Visibility (#983): a runtime model blob takes precedence over the
    // compiled-in predictor — `XGBoostPolicy`/`MlpPolicy::score_row` prefer
    // the runtime cache, and CACHEUS loads its config the same way. When
    // real models are compiled in (`MODELS_AVAILABLE`), a stale or
    // placeholder autoload blob would otherwise silently shadow the real
    // ensemble (the #983 incident: a 90-byte toy XGBoost blob degraded the
    // policy to `first_candidate`). Announce the override so it is never
    // silent; clear an unintended one with `eviction model clear <kind>`.
    // Emitted OUTSIDE the store lock — `log_warn` is UART I/O and must not
    // run under a spinlock. This is the only activation chokepoint (both
    // boot autoload and the shell route through here).
    if super::generated::MODELS_AVAILABLE {
        match kind {
            BlobKind::XGBoost => crate::log::log_warn(
                b"eviction: runtime XGBoost blob active, overriding compiled-in model (clear: eviction model clear xgboost)\n\0",
            ),
            BlobKind::Mlp => crate::log::log_warn(
                b"eviction: runtime MLP blob active, overriding compiled-in model (clear: eviction model clear mlp)\n\0",
            ),
            BlobKind::CacheusConfig => crate::log::log_warn(
                b"eviction: runtime CACHEUS config active, overriding compiled-in ensemble (clear: eviction model clear cacheus_config)\n\0",
            ),
        }
    }
    Ok(())
}

pub fn rollback(kind: BlobKind) -> Result<(), StoreError> {
    let _g = SpinGuard::new();
    unsafe {
        let store = &mut *store_mut(kind);
        let prior = store.rollback.take().ok_or(StoreError::NoRollbackBlob)?;
        store.staged = None;
        let displaced = store.active.replace(prior);
        store.rollback = displaced;
        store.state = SlotState::RolledBack;
    }
    Ok(())
}

pub fn clear(kind: BlobKind) {
    let _g = SpinGuard::new();
    unsafe {
        *store_mut(kind) = KindStore::new();
    }
}

pub fn status(kind: BlobKind) -> BlobStatus {
    let _g = SpinGuard::new();
    unsafe { (*store_ref(kind)).status(kind) }
}

pub fn staged_blob(kind: BlobKind) -> Option<ParsedBlob> {
    let _g = SpinGuard::new();
    unsafe { (*store_ref(kind)).staged.clone() }
}

pub fn active_blob(kind: BlobKind) -> Option<ParsedBlob> {
    let _g = SpinGuard::new();
    unsafe { (*store_ref(kind)).active.clone() }
}
