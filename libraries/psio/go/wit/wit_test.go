package wit

import (
	"encoding/hex"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func buildInventoryWorld() *WitWorld {
	world := &WitWorld{
		Package: "test:inventory@1.0.0",
		Name:    "inventory-api-world",
	}

	// Type 0: record "item"
	world.Types = append(world.Types, WitTypeDef{
		Name: "item",
		Kind: KindRecord,
		Fields: []WitNamedType{
			{Name: "id", TypeIdx: PrimIdx(PrimU64)},
			{Name: "name", TypeIdx: PrimIdx(PrimString)},
			{Name: "category", TypeIdx: PrimIdx(PrimString)},
			{Name: "price-cents", TypeIdx: PrimIdx(PrimU32)},
			{Name: "in-stock", TypeIdx: PrimIdx(PrimBool)},
			{Name: "tags", TypeIdx: 1},          // list<string>
			{Name: "weight-grams", TypeIdx: 2},  // option<u32>
		},
	})

	// Type 1: list<string> (anonymous)
	world.Types = append(world.Types, WitTypeDef{
		Kind:           KindList,
		ElementTypeIdx: PrimIdx(PrimString),
	})

	// Type 2: option<u32> (anonymous)
	world.Types = append(world.Types, WitTypeDef{
		Kind:           KindOption,
		ElementTypeIdx: PrimIdx(PrimU32),
	})

	// Type 3: record "stock-result"
	world.Types = append(world.Types, WitTypeDef{
		Name: "stock-result",
		Kind: KindRecord,
		Fields: []WitNamedType{
			{Name: "item-id", TypeIdx: PrimIdx(PrimU32)},
			{Name: "old-quantity", TypeIdx: PrimIdx(PrimU32)},
			{Name: "new-quantity", TypeIdx: PrimIdx(PrimU32)},
		},
	})

	// Type 4: record "search-query"
	world.Types = append(world.Types, WitTypeDef{
		Name: "search-query",
		Kind: KindRecord,
		Fields: []WitNamedType{
			{Name: "text", TypeIdx: PrimIdx(PrimString)},
			{Name: "max-results", TypeIdx: PrimIdx(PrimU32)},
			{Name: "min-price", TypeIdx: 2},     // option<u32>
			{Name: "max-price", TypeIdx: 2},     // option<u32>
			{Name: "categories", TypeIdx: 1},    // list<string>
		},
	})

	// Type 5: record "search-response"
	world.Types = append(world.Types, WitTypeDef{
		Name: "search-response",
		Kind: KindRecord,
		Fields: []WitNamedType{
			{Name: "items", TypeIdx: 6},         // list<item>
			{Name: "total-count", TypeIdx: PrimIdx(PrimU64)},
			{Name: "has-more", TypeIdx: PrimIdx(PrimBool)},
		},
	})

	// Type 6: list<item> (anonymous, element type index = 0)
	world.Types = append(world.Types, WitTypeDef{
		Kind:           KindList,
		ElementTypeIdx: 0,
	})

	// Type 7: record "bulk-result"
	world.Types = append(world.Types, WitTypeDef{
		Name: "bulk-result",
		Kind: KindRecord,
		Fields: []WitNamedType{
			{Name: "inserted", TypeIdx: PrimIdx(PrimU32)},
			{Name: "failed", TypeIdx: PrimIdx(PrimU32)},
			{Name: "errors", TypeIdx: 1},        // list<string>
		},
	})

	// Type 8: option<item> (anonymous, element type index = 0)
	world.Types = append(world.Types, WitTypeDef{
		Kind:           KindOption,
		ElementTypeIdx: 0,
	})

	// Functions
	world.Funcs = []WitFunc{
		{
			Name:   "get-item",
			Params: []WitNamedType{{Name: "item-id", TypeIdx: PrimIdx(PrimU32)}},
			Results: []WitNamedType{{Name: "", TypeIdx: 8}}, // option<item>
		},
		{
			Name:   "list-items",
			Params: []WitNamedType{{Name: "category", TypeIdx: PrimIdx(PrimString)}},
			Results: []WitNamedType{{Name: "", TypeIdx: 6}}, // list<item>
		},
		{
			Name:   "add-item",
			Params: []WitNamedType{{Name: "item", TypeIdx: 0}}, // record item
			Results: []WitNamedType{{Name: "", TypeIdx: PrimIdx(PrimU64)}},
		},
		{
			Name: "update-stock",
			Params: []WitNamedType{
				{Name: "item-id", TypeIdx: PrimIdx(PrimU32)},
				{Name: "delta", TypeIdx: PrimIdx(PrimS32)},
			},
			Results: []WitNamedType{{Name: "", TypeIdx: 3}}, // stock-result
		},
		{
			Name:   "search",
			Params: []WitNamedType{{Name: "query", TypeIdx: 4}}, // search-query
			Results: []WitNamedType{{Name: "", TypeIdx: 5}},     // search-response
		},
		{
			Name:   "bulk-import",
			Params: []WitNamedType{{Name: "items", TypeIdx: 6}}, // list<item>
			Results: []WitNamedType{{Name: "", TypeIdx: 7}},     // bulk-result
		},
		{
			Name:    "ping",
			Params:  []WitNamedType{},
			Results: []WitNamedType{},
		},
	}

	// Exports: 1 interface
	world.Exports = []WitInterface{
		{
			Name:     "inventory-api",
			TypeIdxs: []uint32{0, 3, 4, 5, 7},
			FuncIdxs: []uint32{0, 1, 2, 3, 4, 5, 6},
		},
	}

	return world
}

func TestEncodeInventoryAPI(t *testing.T) {
	world := buildInventoryWorld()
	got := Encode(world)

	// Read golden binary
	_, thisFile, _, _ := runtime.Caller(0)
	goldenPath := filepath.Join(filepath.Dir(thisFile), "..", "..", "golden", "inventory_api.wasm")
	want, err := os.ReadFile(goldenPath)
	if err != nil {
		t.Fatalf("failed to read golden binary %s: %v", goldenPath, err)
	}

	if len(got) != len(want) {
		t.Errorf("length mismatch: got %d bytes, want %d bytes", len(got), len(want))
	}

	// Find first difference
	minLen := len(got)
	if len(want) < minLen {
		minLen = len(want)
	}

	for i := 0; i < minLen; i++ {
		if got[i] != want[i] {
			// Show context around the difference
			start := i
			if start > 8 {
				start = i - 8
			}
			end := i + 16
			if end > minLen {
				end = minLen
			}

			t.Errorf("first difference at byte offset 0x%04x (%d):", i, i)
			t.Errorf("  got:  ...%s...", hex.EncodeToString(got[start:end]))
			t.Errorf("  want: ...%s...", hex.EncodeToString(want[start:end]))
			t.Errorf("  got[%d]=0x%02x, want[%d]=0x%02x", i, got[i], i, want[i])
			break
		}
	}

	if len(got) != len(want) || !bytesEqual(got, want) {
		// Dump full hex for debugging
		t.Logf("Full got  hex (%d bytes):\n%s", len(got), formatHexDump(got))
		t.Logf("Full want hex (%d bytes):\n%s", len(want), formatHexDump(want))
		t.FailNow()
	}
}

func bytesEqual(a, b []byte) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

func formatHexDump(data []byte) string {
	var result string
	for i := 0; i < len(data); i += 16 {
		end := i + 16
		if end > len(data) {
			end = len(data)
		}
		result += fmt.Sprintf("%04x: %s\n", i, hex.EncodeToString(data[i:end]))
	}
	return result
}
