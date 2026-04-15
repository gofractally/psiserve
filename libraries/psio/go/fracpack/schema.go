//go:generate go run github.com/psibase/psio/fracpack-viewgen -type Point,UserProfile,LineItem,SensorReading,Order -fixed Point -o typed_view_gen.go

package fracpack

// Point is a non-extensible (fixed-layout) struct.
type Point struct {
	X float64
	Y float64
}

type UserProfile struct {
	ID       uint64
	Name     string
	Email    string
	Bio      *string
	Age      uint32
	Score    float64
	Tags     []string
	Verified bool
}

type LineItem struct {
	Product   string
	Qty       uint32
	UnitPrice float64 `fracpack:"unit_price"`
}

type SensorReading struct {
	Timestamp uint64
	DeviceID  string `fracpack:"device_id"`
	Temp      float64
	Humidity  float64
	Pressure  float64
	AccelX    float64 `fracpack:"accel_x"`
	AccelY    float64 `fracpack:"accel_y"`
	AccelZ    float64 `fracpack:"accel_z"`
	GyroX     float64 `fracpack:"gyro_x"`
	GyroY     float64 `fracpack:"gyro_y"`
	GyroZ     float64 `fracpack:"gyro_z"`
	MagX      float64 `fracpack:"mag_x"`
	MagY      float64 `fracpack:"mag_y"`
	MagZ      float64 `fracpack:"mag_z"`
	Battery   float32
	SignalDBM int16 `fracpack:"signal_dbm"`
	ErrorCode *uint32
	Firmware  string
}

type Order struct {
	ID       uint64
	Customer UserProfile
	Items    []LineItem
	Total    float64
	Note     *string
}
