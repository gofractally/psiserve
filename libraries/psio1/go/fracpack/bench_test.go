package fracpack

import (
	"encoding/json"
	"fmt"
	"testing"
)

// sink prevents the compiler from optimizing away benchmark results.
var sink interface{}

// bsink prevents dead-code elimination for []byte results (avoids interface boxing).
var bsink []byte

// ── Schema definitions ──────────────────────────────────────────────────

func pointTypeDef() *TypeDef {
	return &TypeDef{Name: "Point", Fields: []FieldDef{
		{Name: "x", Kind: KindF64, FixedSize: 8},
		{Name: "y", Kind: KindF64, FixedSize: 8},
	}}
}

func tokenTypeDef() *TypeDef {
	return &TypeDef{Name: "Token", Extensible: true, Fields: []FieldDef{
		{Name: "kind", Kind: KindU16, FixedSize: 2},
		{Name: "offset", Kind: KindU32, FixedSize: 4},
		{Name: "length", Kind: KindU32, FixedSize: 4},
		{Name: "text", Kind: KindString, FixedSize: 4, IsVar: true},
	}}
}

func userProfileTypeDef() *TypeDef {
	return &TypeDef{Name: "UserProfile", Extensible: true, Fields: []FieldDef{
		{Name: "id", Kind: KindU64, FixedSize: 8},
		{Name: "name", Kind: KindString, FixedSize: 4, IsVar: true},
		{Name: "email", Kind: KindString, FixedSize: 4, IsVar: true},
		{Name: "bio", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindString},
		{Name: "age", Kind: KindU32, FixedSize: 4},
		{Name: "score", Kind: KindF64, FixedSize: 8},
		{Name: "tags", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindString},
		{Name: "verified", Kind: KindBool, FixedSize: 1},
	}}
}

func lineItemTypeDef() *TypeDef {
	return &TypeDef{Name: "LineItem", Extensible: true, Fields: []FieldDef{
		{Name: "product", Kind: KindString, FixedSize: 4, IsVar: true},
		{Name: "qty", Kind: KindU32, FixedSize: 4},
		{Name: "unit_price", Kind: KindF64, FixedSize: 8},
	}}
}

func orderTypeDef() *TypeDef {
	userTD := userProfileTypeDef()
	liTD := lineItemTypeDef()
	statusTD := &TypeDef{Name: "OrderStatus", IsVariant: true, Cases: []FieldDef{
		{Name: "pending", Kind: KindU32, FixedSize: 4},
		{Name: "shipped", Kind: KindString, FixedSize: 4, IsVar: true},
		{Name: "delivered", Kind: KindU64, FixedSize: 8},
		{Name: "cancelled", Kind: KindString, FixedSize: 4, IsVar: true},
	}}
	return &TypeDef{Name: "Order", Extensible: true, Fields: []FieldDef{
		{Name: "id", Kind: KindU64, FixedSize: 8},
		{Name: "customer", Kind: KindObject, FixedSize: 4, IsVar: true, InnerDef: userTD},
		{Name: "items", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindObject, ElemDef: liTD},
		{Name: "total", Kind: KindF64, FixedSize: 8},
		{Name: "note", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindString},
		{Name: "status", Kind: KindVariant, FixedSize: 4, IsVar: true, InnerDef: statusTD},
	}}
}

func sensorReadingTypeDef() *TypeDef {
	return &TypeDef{Name: "SensorReading", Extensible: true, Fields: []FieldDef{
		{Name: "timestamp", Kind: KindU64, FixedSize: 8},
		{Name: "device_id", Kind: KindString, FixedSize: 4, IsVar: true},
		{Name: "temp", Kind: KindF64, FixedSize: 8},
		{Name: "humidity", Kind: KindF64, FixedSize: 8},
		{Name: "pressure", Kind: KindF64, FixedSize: 8},
		{Name: "accel_x", Kind: KindF64, FixedSize: 8},
		{Name: "accel_y", Kind: KindF64, FixedSize: 8},
		{Name: "accel_z", Kind: KindF64, FixedSize: 8},
		{Name: "gyro_x", Kind: KindF64, FixedSize: 8},
		{Name: "gyro_y", Kind: KindF64, FixedSize: 8},
		{Name: "gyro_z", Kind: KindF64, FixedSize: 8},
		{Name: "mag_x", Kind: KindF64, FixedSize: 8},
		{Name: "mag_y", Kind: KindF64, FixedSize: 8},
		{Name: "mag_z", Kind: KindF64, FixedSize: 8},
		{Name: "battery", Kind: KindF32, FixedSize: 4},
		{Name: "signal_dbm", Kind: KindI16, FixedSize: 2},
		{Name: "error_code", Kind: KindOptional, FixedSize: 4, IsVar: true, ElemKind: KindU32},
		{Name: "firmware", Kind: KindString, FixedSize: 4, IsVar: true},
	}}
}

// ── Sample data builders (matching benchmark_data.json) ─────────────────

func pointValue() *Value {
	return &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindF64, F64: 278.853597},
		{Kind: KindF64, F64: -949.97849},
	}}
}

func tokenValue() *Value {
	return &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU16, U16: 26},
		{Kind: KindU32, U32: 88696},
		{Kind: KindU32, U32: 190},
		{Kind: KindString, Str: "sigma"},
	}}
}

func userProfileValue() *Value {
	return &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU64, U64: 549661728470919412},
		{Kind: KindString, Str: "Tau Xi"},
		{Kind: KindString, Str: "tau.xi@example.com"},
		{Kind: KindOptional, Opt: nil}, // bio = null
		{Kind: KindU32, U32: 82},
		{Kind: KindF64, F64: 60.2},
		{Kind: KindVec, Vec: []Value{
			{Kind: KindString, Str: "eta"},
			{Kind: KindString, Str: "psi"},
			{Kind: KindString, Str: "phi"},
			{Kind: KindString, Str: "psi"},
			{Kind: KindString, Str: "sigma"},
		}},
		{Kind: KindBool, Bool: false},
	}}
}

func orderValue() *Value {
	customer := &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU64, U64: 3000438492122948674},
		{Kind: KindString, Str: "Iota Omicron"},
		{Kind: KindString, Str: "iota.omicron@bench.dev"},
		{Kind: KindOptional, Opt: &Value{Kind: KindString, Str: "How vexingly quick daft zebras jump."}},
		{Kind: KindU32, U32: 27},
		{Kind: KindF64, F64: 60.91},
		{Kind: KindVec, Vec: []Value{
			{Kind: KindString, Str: "sigma"},
			{Kind: KindString, Str: "omega"},
		}},
		{Kind: KindBool, Bool: false},
	}}

	items := []Value{
		lineItemValue("tau iota", 1, 759.04),
		lineItemValue("zeta psi", 55, 340.9),
		lineItemValue("epsilon eta", 98, 337.25),
		lineItemValue("gamma nu", 13, 359.61),
		lineItemValue("mu upsilon", 34, 807.31),
		lineItemValue("omega omicron", 69, 125.69),
		lineItemValue("nu gamma", 71, 293.88),
		lineItemValue("phi upsilon", 47, 577.76),
	}

	return &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU64, U64: 7012091250016467665},
		*customer,
		{Kind: KindVec, Vec: items},
		{Kind: KindF64, F64: 141375.32},
		{Kind: KindOptional, Opt: nil}, // note = null
		{Kind: KindVariant, Tag: 0, Variant: &Value{Kind: KindU32, U32: 0}}, // pending
	}}
}

func lineItemValue(product string, qty uint32, price float64) Value {
	return Value{Kind: KindObject, Fields: []Value{
		{Kind: KindString, Str: product},
		{Kind: KindU32, U32: qty},
		{Kind: KindF64, F64: price},
	}}
}

func sensorReadingValue() *Value {
	return &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindU64, U64: 1789873870754},
		{Kind: KindString, Str: "sensor-9126"},
		{Kind: KindF64, F64: -12.5481},
		{Kind: KindF64, F64: 32.4283},
		{Kind: KindF64, F64: 1026.8314},
		{Kind: KindF64, F64: 3.487602},
		{Kind: KindF64, F64: 1.278881},
		{Kind: KindF64, F64: 10.48662},
		{Kind: KindF64, F64: -0.095272},
		{Kind: KindF64, F64: 0.202219},
		{Kind: KindF64, F64: 0.089307},
		{Kind: KindF64, F64: 17.7435},
		{Kind: KindF64, F64: -3.2589},
		{Kind: KindF64, F64: -38.6318},
		{Kind: KindF32, F32: 4.0},
		{Kind: KindI16, I16: -38},
		{Kind: KindOptional, Opt: nil}, // error_code = null
		{Kind: KindString, Str: "v5.17.252"},
	}}
}

// ── Benchmark entry tables ──────────────────────────────────────────────

type benchCase struct {
	name string
	td   *TypeDef
	val  *Value
}

func benchCases() []benchCase {
	return []benchCase{
		{"Point", pointTypeDef(), pointValue()},
		{"Token", tokenTypeDef(), tokenValue()},
		{"UserProfile", userProfileTypeDef(), userProfileValue()},
		{"Order", orderTypeDef(), orderValue()},
		{"SensorReading", sensorReadingTypeDef(), sensorReadingValue()},
	}
}

// ── 1. Pack benchmarks ──────────────────────────────────────────────────

func BenchmarkPack(b *testing.B) {
	for _, bc := range benchCases() {
		bc := bc
		b.Run(bc.name, func(b *testing.B) {
			b.ReportAllocs()
			packed := Pack(bc.td, bc.val)
			b.SetBytes(int64(len(packed)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				sink = Pack(bc.td, bc.val)
			}
		})
	}
}

// ── 2. Unpack benchmarks ────────────────────────────────────────────────

func BenchmarkUnpack(b *testing.B) {
	for _, bc := range benchCases() {
		bc := bc
		packed := Pack(bc.td, bc.val)
		b.Run(bc.name, func(b *testing.B) {
			b.ReportAllocs()
			b.SetBytes(int64(len(packed)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				sink, _ = Unpack(bc.td, packed)
			}
		})
	}
}

// ── 3. View benchmarks (zero-copy field access) ─────────────────────────

func BenchmarkViewOne(b *testing.B) {
	// Access a single field via the zero-copy view.
	b.Run("UserProfile/name", func(b *testing.B) {
		td := userProfileTypeDef()
		packed := Pack(td, userProfileValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sink = v.Field("name").String()
		}
	})

	b.Run("UserProfile/verified", func(b *testing.B) {
		td := userProfileTypeDef()
		packed := Pack(td, userProfileValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sink = v.Field("verified").Bool()
		}
	})

	b.Run("SensorReading/firmware", func(b *testing.B) {
		td := sensorReadingTypeDef()
		packed := Pack(td, sensorReadingValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sink = v.Field("firmware").String()
		}
	})

	b.Run("Order/customer.name", func(b *testing.B) {
		td := orderTypeDef()
		packed := Pack(td, orderValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			inner := v.Field("customer").Object()
			sink = inner.Field("name").String()
		}
	})
}

func BenchmarkViewAll(b *testing.B) {
	b.Run("Point", func(b *testing.B) {
		td := pointTypeDef()
		packed := Pack(td, pointValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		var sx float64
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sx = v.FieldAt(0).F64() + v.FieldAt(1).F64()
		}
		sink = sx
	})

	// Access every field in the struct via the view.
	b.Run("UserProfile", func(b *testing.B) {
		td := userProfileTypeDef()
		packed := Pack(td, userProfileValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sink = v.Field("id").U64()
			sink = v.Field("name").String()
			sink = v.Field("email").String()
			sink = v.Field("bio").IsPresent()
			sink = v.Field("age").U32()
			sink = v.Field("score").F64()
			vv := v.Field("tags").Vec()
			for j := 0; j < vv.Len(); j++ {
				sink = vv.StringAt(j)
			}
			sink = v.Field("verified").Bool()
		}
	})

	b.Run("SensorReading", func(b *testing.B) {
		td := sensorReadingTypeDef()
		packed := Pack(td, sensorReadingValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sink = v.Field("timestamp").U64()
			sink = v.Field("device_id").String()
			sink = v.Field("temp").F64()
			sink = v.Field("humidity").F64()
			sink = v.Field("pressure").F64()
			sink = v.Field("accel_x").F64()
			sink = v.Field("accel_y").F64()
			sink = v.Field("accel_z").F64()
			sink = v.Field("gyro_x").F64()
			sink = v.Field("gyro_y").F64()
			sink = v.Field("gyro_z").F64()
			sink = v.Field("mag_x").F64()
			sink = v.Field("mag_y").F64()
			sink = v.Field("mag_z").F64()
			sink = v.Field("battery").F32()
			sink = v.Field("signal_dbm").I16()
			sink = v.Field("error_code").IsPresent()
			sink = v.Field("firmware").String()
		}
	})
}

// ── 4. Validate benchmarks ──────────────────────────────────────────────

func BenchmarkValidate(b *testing.B) {
	for _, bc := range benchCases() {
		bc := bc
		packed := Pack(bc.td, bc.val)
		b.Run(bc.name, func(b *testing.B) {
			b.ReportAllocs()
			b.SetBytes(int64(len(packed)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				sink = Validate(bc.td, packed)
			}
		})
	}
}

// ── 5. JSON benchmarks ─────────────────────────────────────────────────

func BenchmarkJSONWrite(b *testing.B) {
	for _, bc := range benchCases() {
		bc := bc
		b.Run(bc.name, func(b *testing.B) {
			b.ReportAllocs()
			jsonBytes, _ := ToJSON(bc.td, bc.val)
			b.SetBytes(int64(len(jsonBytes)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				sink, _ = ToJSON(bc.td, bc.val)
			}
		})
	}
}

func BenchmarkJSONRead(b *testing.B) {
	for _, bc := range benchCases() {
		bc := bc
		jsonBytes, _ := ToJSON(bc.td, bc.val)
		b.Run(bc.name, func(b *testing.B) {
			b.ReportAllocs()
			b.SetBytes(int64(len(jsonBytes)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				sink, _ = FromJSON(bc.td, jsonBytes)
			}
		})
	}
}

func BenchmarkJSONFromPacked(b *testing.B) {
	for _, bc := range benchCases() {
		bc := bc
		packed := Pack(bc.td, bc.val)
		b.Run(bc.name, func(b *testing.B) {
			b.ReportAllocs()
			b.SetBytes(int64(len(packed)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				sink, _ = ToJSONFromPacked(bc.td, packed)
			}
		})
	}
}

// ── 6. Load-Modify-Store (mutation) benchmarks ──────────────────────────

func BenchmarkMutate(b *testing.B) {
	td := userProfileTypeDef()
	val := userProfileValue()

	b.Run("UserProfile/id_scalar", func(b *testing.B) {
		packed := Pack(td, val)
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			buf := make([]byte, len(packed))
			copy(buf, packed)
			mv := NewMutView(&buf, td, false)
			mv.Set("id", uint64(999888777666))
			sink = mv.Bytes()
		}
	})

	b.Run("UserProfile/name_string", func(b *testing.B) {
		packed := Pack(td, val)
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			buf := make([]byte, len(packed))
			copy(buf, packed)
			mv := NewMutView(&buf, td, false)
			mv.Set("name", "Alpha") // same-length replace
			sink = mv.Bytes()
		}
	})

	b.Run("UserProfile/name_grow", func(b *testing.B) {
		packed := Pack(td, val) // name = "Tau Xi" (6 bytes)
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			buf := make([]byte, len(packed))
			copy(buf, packed)
			mv := NewMutView(&buf, td, false)
			mv.Set("name", "Alexandros Papadimitriou the Third") // much longer
			sink = mv.Bytes()
		}
	})

	b.Run("UserProfile/name_shrink", func(b *testing.B) {
		// Start with long name
		longVal := userProfileValue()
		longVal.Fields[1].Str = "Alexandros Papadimitriou the Third"
		packed := Pack(td, longVal)
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			buf := make([]byte, len(packed))
			copy(buf, packed)
			mv := NewMutView(&buf, td, false)
			mv.Set("name", "Al") // shrink
			sink = mv.Bytes()
		}
	})

	// Compare against naive unpack-modify-repack path
	b.Run("UserProfile/naive_unpack_modify_repack", func(b *testing.B) {
		packed := Pack(td, val)
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v, _ := Unpack(td, packed)
			v.Fields[1].Str = "Alpha"
			sink = Pack(td, v)
		}
	})
}

func BenchmarkMutateFast(b *testing.B) {
	td := userProfileTypeDef()
	val := userProfileValue()

	b.Run("UserProfile/name_string", func(b *testing.B) {
		packed := Pack(td, val)
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			buf := make([]byte, len(packed))
			copy(buf, packed)
			mv := NewMutView(&buf, td, true) // fast mode
			mv.Set("name", "Alpha")
			sink = mv.Bytes()
		}
	})

	b.Run("UserProfile/name_grow", func(b *testing.B) {
		packed := Pack(td, val)
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			buf := make([]byte, len(packed))
			copy(buf, packed)
			mv := NewMutView(&buf, td, true)
			mv.Set("name", "Alexandros Papadimitriou the Third")
			sink = mv.Bytes()
		}
	})
}

// ── 7. Competitor: encoding/json ────────────────────────────────────────

// Native Go structs for encoding/json comparison.

type jsonPoint struct {
	X float64 `json:"x"`
	Y float64 `json:"y"`
}

type jsonToken struct {
	Kind   uint16 `json:"kind"`
	Offset uint32 `json:"offset"`
	Length uint32 `json:"length"`
	Text   string `json:"text"`
}

type jsonUserProfile struct {
	ID       uint64   `json:"id"`
	Name     string   `json:"name"`
	Email    string   `json:"email"`
	Bio      *string  `json:"bio"`
	Age      uint32   `json:"age"`
	Score    float64  `json:"score"`
	Tags     []string `json:"tags"`
	Verified bool     `json:"verified"`
}

type jsonLineItem struct {
	Product   string  `json:"product"`
	Qty       uint32  `json:"qty"`
	UnitPrice float64 `json:"unit_price"`
}

type jsonSensorReading struct {
	Timestamp uint64  `json:"timestamp"`
	DeviceID  string  `json:"device_id"`
	Temp      float64 `json:"temp"`
	Humidity  float64 `json:"humidity"`
	Pressure  float64 `json:"pressure"`
	AccelX    float64 `json:"accel_x"`
	AccelY    float64 `json:"accel_y"`
	AccelZ    float64 `json:"accel_z"`
	GyroX     float64 `json:"gyro_x"`
	GyroY     float64 `json:"gyro_y"`
	GyroZ     float64 `json:"gyro_z"`
	MagX      float64 `json:"mag_x"`
	MagY      float64 `json:"mag_y"`
	MagZ      float64 `json:"mag_z"`
	Battery   float32 `json:"battery"`
	SignalDBM int16   `json:"signal_dbm"`
	ErrorCode *uint32 `json:"error_code"`
	Firmware  string  `json:"firmware"`
}

type jsonCompCase struct {
	name      string
	nativeVal interface{}
}

func jsonCompCases() []jsonCompCase {
	return []jsonCompCase{
		{"Point", jsonPoint{X: 278.853597, Y: -949.97849}},
		{"Token", jsonToken{Kind: 26, Offset: 88696, Length: 190, Text: "sigma"}},
		{"UserProfile", jsonUserProfile{
			ID: 549661728470919412, Name: "Tau Xi",
			Email: "tau.xi@example.com", Bio: nil, Age: 82, Score: 60.2,
			Tags: []string{"eta", "psi", "phi", "psi", "sigma"}, Verified: false,
		}},
		{"SensorReading", jsonSensorReading{
			Timestamp: 1789873870754, DeviceID: "sensor-9126",
			Temp: -12.5481, Humidity: 32.4283, Pressure: 1026.8314,
			AccelX: 3.487602, AccelY: 1.278881, AccelZ: 10.48662,
			GyroX: -0.095272, GyroY: 0.202219, GyroZ: 0.089307,
			MagX: 17.7435, MagY: -3.2589, MagZ: -38.6318,
			Battery: 4.0, SignalDBM: -38, ErrorCode: nil, Firmware: "v5.17.252",
		}},
	}
}

func BenchmarkCompetitor_JSON(b *testing.B) {
	b.Run("Marshal", func(b *testing.B) {
		for _, cc := range jsonCompCases() {
			cc := cc
			b.Run(cc.name, func(b *testing.B) {
				b.ReportAllocs()
				data, _ := json.Marshal(cc.nativeVal)
				b.SetBytes(int64(len(data)))
				b.ResetTimer()
				for i := 0; i < b.N; i++ {
					sink, _ = json.Marshal(cc.nativeVal)
				}
			})
		}
	})

	b.Run("Unmarshal", func(b *testing.B) {
		for _, cc := range jsonCompCases() {
			cc := cc
			data, _ := json.Marshal(cc.nativeVal)
			b.Run(cc.name, func(b *testing.B) {
				b.ReportAllocs()
				b.SetBytes(int64(len(data)))
				b.ResetTimer()
				for i := 0; i < b.N; i++ {
					switch cc.name {
					case "Point":
						var v jsonPoint
						json.Unmarshal(data, &v)
						sink = v
					case "Token":
						var v jsonToken
						json.Unmarshal(data, &v)
						sink = v
					case "UserProfile":
						var v jsonUserProfile
						json.Unmarshal(data, &v)
						sink = v
					case "SensorReading":
						var v jsonSensorReading
						json.Unmarshal(data, &v)
						sink = v
					}
				}
			})
		}
	})
}

// ── 8. Array scaling benchmarks ─────────────────────────────────────────

func pointCloudTypeDef() *TypeDef {
	ptTD := pointTypeDef()
	return &TypeDef{Name: "PointCloud", Extensible: true, Fields: []FieldDef{
		{Name: "points", Kind: KindVec, FixedSize: 4, IsVar: true, ElemKind: KindObject, ElemDef: ptTD},
	}}
}

func makePointCloud(n int) *Value {
	points := make([]Value, n)
	for i := range points {
		x := float64(i)*1.23 - 500.0
		y := float64(i)*0.97 + 100.0
		points[i] = Value{Kind: KindObject, Fields: []Value{
			{Kind: KindF64, F64: x},
			{Kind: KindF64, F64: y},
		}}
	}
	return &Value{Kind: KindObject, Fields: []Value{
		{Kind: KindVec, Vec: points},
	}}
}

func BenchmarkArrayScale(b *testing.B) {
	td := pointCloudTypeDef()

	for _, n := range []int{10, 100, 1000, 10000} {
		val := makePointCloud(n)
		packed := Pack(td, val)

		b.Run(fmt.Sprintf("PackPoints/%d", n), func(b *testing.B) {
			b.ReportAllocs()
			b.SetBytes(int64(len(packed)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				sink = Pack(td, val)
			}
		})

		b.Run(fmt.Sprintf("UnpackPoints/%d", n), func(b *testing.B) {
			b.ReportAllocs()
			b.SetBytes(int64(len(packed)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				sink, _ = Unpack(td, packed)
			}
		})

		b.Run(fmt.Sprintf("ValidatePoints/%d", n), func(b *testing.B) {
			b.ReportAllocs()
			b.SetBytes(int64(len(packed)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				sink = Validate(td, packed)
			}
		})

		// View: access last point to measure O(1) vs O(n) advantage
		b.Run(fmt.Sprintf("ViewLastPoint/%d", n), func(b *testing.B) {
			b.ReportAllocs()
			b.SetBytes(int64(len(packed)))
			b.ResetTimer()
			for i := 0; i < b.N; i++ {
				v := NewView(td, packed)
				vv := v.Field("points").Vec()
				last := vv.Len() - 1
				pt := vv.ObjectAt(last)
				sink = pt.Field("x").F64()
			}
		})
	}
}

// ── 9. View vs Unpack comparison ────────────────────────────────────────

func BenchmarkViewVsUnpack(b *testing.B) {
	// Demonstrates the fracpack advantage: accessing one field via view
	// vs full unpack to get the same field.
	td := userProfileTypeDef()
	packed := Pack(td, userProfileValue())

	b.Run("ViewOneField", func(b *testing.B) {
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sink = v.Field("name").String()
		}
	})

	b.Run("UnpackThenAccess", func(b *testing.B) {
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			val, _ := Unpack(td, packed)
			sink = val.Fields[1].Str
		}
	})
}

// ── 10. View vs Native struct access ───────────────────────────────────

// Native Go structs (no tags, no fracpack overhead) for baseline comparison.

type NativePoint struct {
	X, Y float64
}

type NativeUserProfile struct {
	ID       uint64
	Name     string
	Email    string
	Bio      *string
	Age      uint32
	Score    float64
	Tags     []string
	Verified bool
}

type NativeSensorReading struct {
	Timestamp uint64
	DeviceID  string
	Temp      float64
	Humidity  float64
	Pressure  float64
	AccelX    float64
	AccelY    float64
	AccelZ    float64
	GyroX     float64
	GyroY     float64
	GyroZ     float64
	MagX      float64
	MagY      float64
	MagZ      float64
	Battery   float32
	SignalDBM int16
	ErrorCode *uint32
	Firmware  string
}

type NativeLineItem struct {
	Product   string
	Qty       uint32
	UnitPrice float64
}

type NativeOrder struct {
	ID       uint64
	Customer NativeUserProfile
	Items    []NativeLineItem
	Total    float64
	Note     *string
	// Status omitted — variant types don't have a direct native analog
}

func BenchmarkViewVsNative(b *testing.B) {

	// ── Point (fixed-size, should be near-equal) ──

	b.Run("Point/native", func(b *testing.B) {
		p := NativePoint{X: 278.853597, Y: -949.97849}
		b.ReportAllocs()
		b.ResetTimer()
		var sx float64
		for i := 0; i < b.N; i++ {
			sx = p.X + p.Y
		}
		sink = sx
	})

	b.Run("Point/view", func(b *testing.B) {
		td := pointTypeDef()
		packed := Pack(td, pointValue())
		td.initOffsets() // pre-compute offsets outside the loop
		b.ReportAllocs()
		b.ResetTimer()
		var sx float64
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sx = v.FieldAt(0).F64() + v.FieldAt(1).F64()
		}
		sink = sx
	})

	b.Run("Point/unpack_then_access", func(b *testing.B) {
		td := pointTypeDef()
		packed := Pack(td, pointValue())
		b.ReportAllocs()
		b.ResetTimer()
		var sx float64
		for i := 0; i < b.N; i++ {
			val, _ := Unpack(td, packed)
			sx = val.Fields[0].F64 + val.Fields[1].F64
		}
		sink = sx
	})

	// ── UserProfile (complex type with strings, optional, vector) ──

	b.Run("UserProfile/native", func(b *testing.B) {
		u := NativeUserProfile{
			ID: 549661728470919412, Name: "Tau Xi",
			Email: "tau.xi@example.com", Bio: nil, Age: 82, Score: 60.2,
			Tags: []string{"eta", "psi", "phi", "psi", "sigma"}, Verified: false,
		}
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			sink = u.ID
			sink = u.Name
			sink = u.Email
			sink = u.Bio
			sink = u.Age
			sink = u.Score
			sink = u.Tags
			sink = u.Verified
		}
	})

	b.Run("UserProfile/view", func(b *testing.B) {
		td := userProfileTypeDef()
		packed := Pack(td, userProfileValue())
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sink = v.Field("id").U64()
			bsink = v.Field("name").Bytes()
			bsink = v.Field("email").Bytes()
			sink = v.Field("bio").IsPresent()
			sink = v.Field("age").U32()
			sink = v.Field("score").F64()
			vv := v.Field("tags").Vec()
			for j := 0; j < vv.Len(); j++ {
				bsink = vv.BytesAt(j)
			}
			sink = v.Field("verified").Bool()
		}
	})

	b.Run("UserProfile/unpack_then_access", func(b *testing.B) {
		td := userProfileTypeDef()
		packed := Pack(td, userProfileValue())
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			val, _ := Unpack(td, packed)
			sink = val.Fields[0].U64
			sink = val.Fields[1].Str
			sink = val.Fields[2].Str
			sink = val.Fields[3].Opt
			sink = val.Fields[4].U32
			sink = val.Fields[5].F64
			for _, t := range val.Fields[6].Vec {
				sink = t.Str
			}
			sink = val.Fields[7].Bool
		}
	})

	// ── SensorReading (wide struct, 17 fields, mostly fixed) ──

	b.Run("SensorReading/native", func(b *testing.B) {
		s := NativeSensorReading{
			Timestamp: 1789873870754, DeviceID: "sensor-9126",
			Temp: -12.5481, Humidity: 32.4283, Pressure: 1026.8314,
			AccelX: 3.487602, AccelY: 1.278881, AccelZ: 10.48662,
			GyroX: -0.095272, GyroY: 0.202219, GyroZ: 0.089307,
			MagX: 17.7435, MagY: -3.2589, MagZ: -38.6318,
			Battery: 4.0, SignalDBM: -38, ErrorCode: nil, Firmware: "v5.17.252",
		}
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			sink = s.Timestamp
			sink = s.DeviceID
			sink = s.Temp
			sink = s.Humidity
			sink = s.Pressure
			sink = s.AccelX
			sink = s.AccelY
			sink = s.AccelZ
			sink = s.GyroX
			sink = s.GyroY
			sink = s.GyroZ
			sink = s.MagX
			sink = s.MagY
			sink = s.MagZ
			sink = s.Battery
			sink = s.SignalDBM
			sink = s.ErrorCode
			sink = s.Firmware
		}
	})

	b.Run("SensorReading/view", func(b *testing.B) {
		td := sensorReadingTypeDef()
		packed := Pack(td, sensorReadingValue())
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sink = v.Field("timestamp").U64()
			bsink = v.Field("device_id").Bytes()
			sink = v.Field("temp").F64()
			sink = v.Field("humidity").F64()
			sink = v.Field("pressure").F64()
			sink = v.Field("accel_x").F64()
			sink = v.Field("accel_y").F64()
			sink = v.Field("accel_z").F64()
			sink = v.Field("gyro_x").F64()
			sink = v.Field("gyro_y").F64()
			sink = v.Field("gyro_z").F64()
			sink = v.Field("mag_x").F64()
			sink = v.Field("mag_y").F64()
			sink = v.Field("mag_z").F64()
			sink = v.Field("battery").F32()
			sink = v.Field("signal_dbm").I16()
			sink = v.Field("error_code").IsPresent()
			bsink = v.Field("firmware").Bytes()
		}
	})

	b.Run("SensorReading/unpack_then_access", func(b *testing.B) {
		td := sensorReadingTypeDef()
		packed := Pack(td, sensorReadingValue())
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			val, _ := Unpack(td, packed)
			sink = val.Fields[0].U64
			sink = val.Fields[1].Str
			sink = val.Fields[2].F64
			sink = val.Fields[3].F64
			sink = val.Fields[4].F64
			sink = val.Fields[5].F64
			sink = val.Fields[6].F64
			sink = val.Fields[7].F64
			sink = val.Fields[8].F64
			sink = val.Fields[9].F64
			sink = val.Fields[10].F64
			sink = val.Fields[11].F64
			sink = val.Fields[12].F64
			sink = val.Fields[13].F64
			sink = val.Fields[14].F32
			sink = val.Fields[15].I16
			sink = val.Fields[16].Opt
			sink = val.Fields[17].Str
		}
	})

	// ── Order (deeply nested: object + vec<object> + variant) ──

	b.Run("Order/native", func(b *testing.B) {
		bio := "How vexingly quick daft zebras jump."
		o := NativeOrder{
			ID: 7012091250016467665,
			Customer: NativeUserProfile{
				ID: 3000438492122948674, Name: "Iota Omicron",
				Email: "iota.omicron@bench.dev", Bio: &bio,
				Age: 27, Score: 60.91,
				Tags: []string{"sigma", "omega"}, Verified: false,
			},
			Items: []NativeLineItem{
				{"tau iota", 1, 759.04},
				{"zeta psi", 55, 340.9},
				{"epsilon eta", 98, 337.25},
				{"gamma nu", 13, 359.61},
				{"mu upsilon", 34, 807.31},
				{"omega omicron", 69, 125.69},
				{"nu gamma", 71, 293.88},
				{"phi upsilon", 47, 577.76},
			},
			Total: 141375.32,
			Note:  nil,
		}
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			sink = o.ID
			sink = o.Customer.ID
			sink = o.Customer.Name
			sink = o.Customer.Email
			sink = o.Customer.Bio
			sink = o.Customer.Age
			sink = o.Customer.Score
			sink = o.Customer.Tags
			sink = o.Customer.Verified
			// Item access omitted (matches view benchmark —
			// Vec<Object> view not yet supported)
			sink = o.Total
			sink = o.Note
		}
	})

	b.Run("Order/view", func(b *testing.B) {
		td := orderTypeDef()
		packed := Pack(td, orderValue())
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewView(td, packed)
			sink = v.Field("id").U64()
			cust := v.Field("customer").Object()
			sink = cust.Field("id").U64()
			bsink = cust.Field("name").Bytes()
			bsink = cust.Field("email").Bytes()
			sink = cust.Field("bio").IsPresent()
			sink = cust.Field("age").U32()
			sink = cust.Field("score").F64()
			tags := cust.Field("tags").Vec()
			for j := 0; j < tags.Len(); j++ {
				bsink = tags.BytesAt(j)
			}
			sink = cust.Field("verified").Bool()
			// Note: Vec<Object> uses offset slots, but VecView.ObjectAt
			// walks contiguously — skip item access until ObjectAt is
			// updated for offset-based element layout.
			sink = v.Field("total").F64()
			sink = v.Field("note").IsPresent()
			vr := v.Field("status").Variant()
			sink = vr.Tag()
		}
	})

	b.Run("Order/unpack_then_access", func(b *testing.B) {
		td := orderTypeDef()
		packed := Pack(td, orderValue())
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			val, _ := Unpack(td, packed)
			sink = val.Fields[0].U64
			cust := val.Fields[1]
			sink = cust.Fields[0].U64
			sink = cust.Fields[1].Str
			sink = cust.Fields[2].Str
			sink = cust.Fields[3].Opt
			sink = cust.Fields[4].U32
			sink = cust.Fields[5].F64
			for _, t := range cust.Fields[6].Vec {
				sink = t.Str
			}
			sink = cust.Fields[7].Bool
			// Item access omitted (matches view benchmark)
			sink = val.Fields[3].F64
			sink = val.Fields[4].Opt
			sink = val.Fields[5].Tag
		}
	})
}

// ── 11. Typed view benchmarks (proxy-style accessors) ─────────────────

func BenchmarkTypedViewOne(b *testing.B) {
	b.Run("UserProfile/name", func(b *testing.B) {
		td := userProfileTypeDef()
		packed := Pack(td, userProfileValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewUserProfileView(packed)
			sink = v.Name()
		}
	})

	b.Run("UserProfile/verified", func(b *testing.B) {
		td := userProfileTypeDef()
		packed := Pack(td, userProfileValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewUserProfileView(packed)
			sink = v.Verified()
		}
	})

	b.Run("SensorReading/firmware", func(b *testing.B) {
		td := sensorReadingTypeDef()
		packed := Pack(td, sensorReadingValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewSensorReadingView(packed)
			sink = v.Firmware()
		}
	})

	b.Run("Order/customer.name", func(b *testing.B) {
		td := orderTypeDef()
		packed := Pack(td, orderValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewOrderView(packed)
			sink = v.Customer().Name()
		}
	})
}

func BenchmarkTypedViewAll(b *testing.B) {
	b.Run("Point", func(b *testing.B) {
		td := pointTypeDef()
		packed := Pack(td, pointValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		var sx float64
		for i := 0; i < b.N; i++ {
			v := NewPointView(packed)
			sx = v.X() + v.Y()
		}
		sink = sx
	})

	b.Run("UserProfile", func(b *testing.B) {
		td := userProfileTypeDef()
		packed := Pack(td, userProfileValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewUserProfileView(packed)
			sink = v.ID()
			bsink = v.NameBytes()
			bsink = v.EmailBytes()
			sink = v.BioPresent()
			sink = v.Age()
			sink = v.Score()
			n := v.TagsLen()
			for j := 0; j < n; j++ {
				bsink = v.TagsBytesAt(j)
			}
			sink = v.Verified()
		}
	})

	b.Run("SensorReading", func(b *testing.B) {
		td := sensorReadingTypeDef()
		packed := Pack(td, sensorReadingValue())
		b.ReportAllocs()
		b.SetBytes(int64(len(packed)))
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewSensorReadingView(packed)
			sink = v.Timestamp()
			bsink = v.DeviceIDBytes()
			sink = v.Temp()
			sink = v.Humidity()
			sink = v.Pressure()
			sink = v.AccelX()
			sink = v.AccelY()
			sink = v.AccelZ()
			sink = v.GyroX()
			sink = v.GyroY()
			sink = v.GyroZ()
			sink = v.MagX()
			sink = v.MagY()
			sink = v.MagZ()
			sink = v.Battery()
			sink = v.SignalDBM()
			sink = v.ErrorCodePresent()
			bsink = v.FirmwareBytes()
		}
	})
}

func BenchmarkTypedViewVsNative(b *testing.B) {
	b.Run("Point/native", func(b *testing.B) {
		p := NativePoint{X: 278.853597, Y: -949.97849}
		b.ReportAllocs()
		b.ResetTimer()
		var sx float64
		for i := 0; i < b.N; i++ {
			sx = p.X + p.Y
		}
		sink = sx
	})

	b.Run("Point/typed_view", func(b *testing.B) {
		td := pointTypeDef()
		packed := Pack(td, pointValue())
		b.ReportAllocs()
		b.ResetTimer()
		var sx float64
		for i := 0; i < b.N; i++ {
			v := NewPointView(packed)
			sx = v.X() + v.Y()
		}
		sink = sx
	})

	b.Run("UserProfile/native", func(b *testing.B) {
		u := NativeUserProfile{
			ID: 549661728470919412, Name: "Tau Xi",
			Email: "tau.xi@example.com", Bio: nil, Age: 82, Score: 60.2,
			Tags: []string{"eta", "psi", "phi", "psi", "sigma"}, Verified: false,
		}
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			sink = u.ID
			sink = u.Name
			sink = u.Email
			sink = u.Bio
			sink = u.Age
			sink = u.Score
			sink = u.Tags
			sink = u.Verified
		}
	})

	b.Run("UserProfile/typed_view", func(b *testing.B) {
		td := userProfileTypeDef()
		packed := Pack(td, userProfileValue())
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewUserProfileView(packed)
			sink = v.ID()
			bsink = v.NameBytes()
			bsink = v.EmailBytes()
			sink = v.BioPresent()
			sink = v.Age()
			sink = v.Score()
			n := v.TagsLen()
			for j := 0; j < n; j++ {
				bsink = v.TagsBytesAt(j)
			}
			sink = v.Verified()
		}
	})

	b.Run("SensorReading/native", func(b *testing.B) {
		s := NativeSensorReading{
			Timestamp: 1789873870754, DeviceID: "sensor-9126",
			Temp: -12.5481, Humidity: 32.4283, Pressure: 1026.8314,
			AccelX: 3.487602, AccelY: 1.278881, AccelZ: 10.48662,
			GyroX: -0.095272, GyroY: 0.202219, GyroZ: 0.089307,
			MagX: 17.7435, MagY: -3.2589, MagZ: -38.6318,
			Battery: 4.0, SignalDBM: -38, ErrorCode: nil, Firmware: "v5.17.252",
		}
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			sink = s.Timestamp
			sink = s.DeviceID
			sink = s.Temp
			sink = s.Humidity
			sink = s.Pressure
			sink = s.AccelX
			sink = s.AccelY
			sink = s.AccelZ
			sink = s.GyroX
			sink = s.GyroY
			sink = s.GyroZ
			sink = s.MagX
			sink = s.MagY
			sink = s.MagZ
			sink = s.Battery
			sink = s.SignalDBM
			sink = s.ErrorCode
			sink = s.Firmware
		}
	})

	b.Run("SensorReading/typed_view", func(b *testing.B) {
		td := sensorReadingTypeDef()
		packed := Pack(td, sensorReadingValue())
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewSensorReadingView(packed)
			sink = v.Timestamp()
			bsink = v.DeviceIDBytes()
			sink = v.Temp()
			sink = v.Humidity()
			sink = v.Pressure()
			sink = v.AccelX()
			sink = v.AccelY()
			sink = v.AccelZ()
			sink = v.GyroX()
			sink = v.GyroY()
			sink = v.GyroZ()
			sink = v.MagX()
			sink = v.MagY()
			sink = v.MagZ()
			sink = v.Battery()
			sink = v.SignalDBM()
			sink = v.ErrorCodePresent()
			bsink = v.FirmwareBytes()
		}
	})

	b.Run("Order/native", func(b *testing.B) {
		bio := "How vexingly quick daft zebras jump."
		o := NativeOrder{
			ID: 7012091250016467665,
			Customer: NativeUserProfile{
				ID: 3000438492122948674, Name: "Iota Omicron",
				Email: "iota.omicron@bench.dev", Bio: &bio,
				Age: 27, Score: 60.91,
				Tags: []string{"sigma", "omega"}, Verified: false,
			},
			Items: []NativeLineItem{
				{"tau iota", 1, 759.04},
				{"zeta psi", 55, 340.9},
				{"epsilon eta", 98, 337.25},
				{"gamma nu", 13, 359.61},
				{"mu upsilon", 34, 807.31},
				{"omega omicron", 69, 125.69},
				{"nu gamma", 71, 293.88},
				{"phi upsilon", 47, 577.76},
			},
			Total: 141375.32,
			Note:  nil,
		}
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			sink = o.ID
			sink = o.Customer.ID
			sink = o.Customer.Name
			sink = o.Customer.Email
			sink = o.Customer.Bio
			sink = o.Customer.Age
			sink = o.Customer.Score
			sink = o.Customer.Tags
			sink = o.Customer.Verified
			sink = o.Total
			sink = o.Note
		}
	})

	b.Run("Order/typed_view", func(b *testing.B) {
		td := orderTypeDef()
		packed := Pack(td, orderValue())
		b.ReportAllocs()
		b.ResetTimer()
		for i := 0; i < b.N; i++ {
			v := NewOrderView(packed)
			sink = v.ID()
			cust := v.Customer()
			sink = cust.ID()
			bsink = cust.NameBytes()
			bsink = cust.EmailBytes()
			sink = cust.BioPresent()
			sink = cust.Age()
			sink = cust.Score()
			n := cust.TagsLen()
			for j := 0; j < n; j++ {
				bsink = cust.TagsBytesAt(j)
			}
			sink = cust.Verified()
			sink = v.Total()
			sink = v.NotePresent()
		}
	})
}
