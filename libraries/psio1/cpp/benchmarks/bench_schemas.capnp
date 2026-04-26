@0xd4a3c6f1e2b50a91;

struct Point {
   x @0 :Float64;
   y @1 :Float64;
}

struct Token {
   kind   @0 :UInt16;
   offset @1 :UInt32;
   length @2 :UInt32;
   text   @3 :Text;
}

struct UserProfile {
   id       @0 :UInt64;
   name     @1 :Text;
   email    @2 :Text;
   bio      @3 :Text;    # optional modeled as empty string vs present
   age      @4 :UInt32;
   score    @5 :Float64;
   tags     @6 :List(Text);
   verified @7 :Bool;
}

struct LineItem {
   product   @0 :Text;
   qty       @1 :UInt32;
   unitPrice @2 :Float64;
}

struct Order {
   id       @0 :UInt64;
   customer @1 :UserProfile;
   items    @2 :List(LineItem);
   total    @3 :Float64;
   note     @4 :Text;    # optional modeled as empty string vs present
}

struct SensorReading {
   timestamp @0  :UInt64;
   deviceId  @1  :Text;
   temp      @2  :Float64;
   humidity  @3  :Float64;
   pressure  @4  :Float64;
   accelX    @5  :Float64;
   accelY    @6  :Float64;
   accelZ    @7  :Float64;
   gyroX     @8  :Float64;
   gyroY     @9  :Float64;
   gyroZ     @10 :Float64;
   magX      @11 :Float64;
   magY      @12 :Float64;
   magZ      @13 :Float64;
   battery   @14 :Float32;
   signalDbm @15 :Int16;
   errorCode @16 :UInt32;  # 0 = not set
   firmware  @17 :Text;
}
