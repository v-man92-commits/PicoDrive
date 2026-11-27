#pragma once
#include <nds/ndstypes.h>
#include "cdLayout.h"

/// SCSI/MMC commands that make a mounted ISO look like a real USB CD/DVD-ROM
/// drive (READ TOC, GET CONFIGURATION, GET EVENT STATUS NOTIFICATION, ...).
/// Data reads (READ 10/12, READ CD) go through the normal image read path.

typedef bool (*OpticalReadFn)(u32 block, u32 count, u8* buffer); // 2048-byte blocks

struct OpticalState
{
    u32 blocks;           // disc size in blocks (frames) = lead-out LBA
    bool present;         // medium loaded (not ejected)
    u8 pendingEvent;      // media event to report: 0 none, 2 new media, 3 removal
    const cd_layout_t* layout; // BIN/CUE disc (raw frames, tracks), or null: ISO (one data track)
};

/// Media events: call when a disc is inserted (mounted) or removed (ejected).
/// layout: a BIN/CUE disc, or null for a plain 2048-byte ISO of `blocks` blocks.
void optical_insert(OpticalState& state, u32 blocks, const cd_layout_t* layout = nullptr);

/// A data read on a BIN/CUE disc (READ 10/12, READ CD, READ CD MSF): which
/// blocks, and which bytes of each frame make up one block.
struct OpticalReadPlan
{
    u32 lba;            // first disc LBA
    u32 count;          // blocks
    u32 blockSize;      // bytes per block sent to the host
    u16 from;           // where a block starts within the frame stored in the image
    u32 cmdLbaField;    // bytes 2-5 of the CDB (TinyUSB adds the block index to this)
};

/// Works out a read on a BIN/CUE disc. False (with sense set) if the command
/// asks for something the tracks can't give (audio through READ 10, wrong
/// sector type, raw data from a 2048-byte track, out of range, ...).
bool optical_planRead(const OpticalState& state, const u8 cdb[16], OpticalReadPlan& plan,
    void (*setSense)(u8 key, u8 asc, u8 ascq));

/// Reads `length` bytes of the virtual image (see cdLayout.h). Returns a
/// pointer to them (valid until the next call) or null on a read error.
typedef const u8* (*OpticalSpanFn)(u32 byteOffset, u32 length);

/// Produces `length` bytes of a planned read, starting `offsetInBlock` bytes
/// into block `blockIndex` of the plan. Frames outside the stored data
/// (pregaps given as PREGAP) read as zeros. maxSpan: the most `span` can read.
bool optical_produce(const OpticalState& state, const OpticalReadPlan& plan, u32 blockIndex,
    u32 offsetInBlock, u8* out, u32 length, OpticalSpanFn span, u32 maxSpan);
void optical_remove(OpticalState& state);

/// DVD-ROM profile for images bigger than a CD, CD-ROM profile otherwise.
// READ(12) / READ CD (user data only) that TinyUSB should stream through
// tud_msc_read10_cb(): returns the block count, or 0 to handle it as a normal
// command (bad fields, out of range, other sector formats, no disc).
u32 optical_streamBlocks(const OpticalState& state, const u8 cdb[16], u32 totalBytes);

bool optical_isDvd(const OpticalState& state);

/// The reader optical_scsi uses for BIN/CUE reads that aren't streamed.
void optical_setSpanReader(OpticalSpanFn span, u32 maxSpan);

/// Handles one SCSI command for the optical drive.
/// Returns the response length (>= 0), or -1 after setting sense data
/// through the provided callback. `handled` is false for commands the
/// optical layer doesn't implement (so the caller can fall back).
int32_t optical_scsi(OpticalState& state, const u8 cdb[16], u8* buffer, u32 bufsize,
    OpticalReadFn read, void (*setSense)(u8 key, u8 asc, u8 ascq), bool& handled);
