package fracpack

// Runtime helpers for generated typed view code.
//
// Generated view structs call these functions to read scalar and
// variable-length fields from packed buffers at known byte offsets.
// Unexported versions are used by views generated into the fracpack
// package itself; exported versions are used by external packages.

import (
	"encoding/binary"
	"math"
)

// ── exported (for generated code in external packages) ──────────────────

func ViewReadU16(b []byte, off uint32) uint16  { return binary.LittleEndian.Uint16(b[off:]) }
func ViewReadU32(b []byte, off uint32) uint32  { return binary.LittleEndian.Uint32(b[off:]) }
func ViewReadU64(b []byte, off uint32) uint64  { return binary.LittleEndian.Uint64(b[off:]) }
func ViewReadI8(b []byte, off uint32) int8     { return int8(b[off]) }
func ViewReadI16(b []byte, off uint32) int16   { return int16(binary.LittleEndian.Uint16(b[off:])) }
func ViewReadI32(b []byte, off uint32) int32   { return int32(binary.LittleEndian.Uint32(b[off:])) }
func ViewReadI64(b []byte, off uint32) int64   { return int64(binary.LittleEndian.Uint64(b[off:])) }
func ViewReadF32(b []byte, off uint32) float32 { return math.Float32frombits(ViewReadU32(b, off)) }
func ViewReadF64(b []byte, off uint32) float64 { return math.Float64frombits(ViewReadU64(b, off)) }

// ViewReadStr follows a u32 relative offset at pos and returns the
// string bytes as a zero-copy slice into the buffer.
func ViewReadStr(b []byte, pos uint32) []byte {
	off := ViewReadU32(b, pos)
	if off == 0 {
		return nil
	}
	hp := pos + off
	n := ViewReadU32(b, hp)
	if n == 0 {
		return nil
	}
	return b[hp+4 : hp+4+n]
}
