#pragma once

#define PSIZAM_REPEAT_0(m)
#define PSIZAM_REPEAT_1(m) m(0)
#define PSIZAM_REPEAT_2(m) m(0) m(1)
#define PSIZAM_REPEAT_3(m) m(0) m(1) m(2)
#define PSIZAM_REPEAT_4(m) m(0) m(1) m(2) m(3)
#define PSIZAM_REPEAT_5(m) PSIZAM_REPEAT_1(m) m(1) m(2) m(3) m(4)
#define PSIZAM_REPEAT_6(m) PSIZAM_REPEAT_2(m) m(2) m(3) m(4) m(5)
#define PSIZAM_REPEAT_7(m) PSIZAM_REPEAT_3(m) m(3) m(4) m(5) m(6)
#define PSIZAM_REPEAT_8(m) PSIZAM_REPEAT_4(m) m(4) m(5) m(6) m(7)
#define PSIZAM_REPEAT_9(m) PSIZAM_REPEAT_5(m) m(5) m(6) m(7) m(8)
#define PSIZAM_REPEAT_10(m) PSIZAM_REPEAT_6(m) m(6) m(7) m(8) m(9)
#define PSIZAM_REPEAT_11(m) PSIZAM_REPEAT_7(m) m(7) m(8) m(9) m(10)
#define PSIZAM_REPEAT_12(m) PSIZAM_REPEAT_8(m) m(8) m(9) m(10) m(11)
#define PSIZAM_REPEAT_13(m) PSIZAM_REPEAT_9(m) m(9) m(10) m(11) m(12)
#define PSIZAM_REPEAT_14(m) PSIZAM_REPEAT_10(m) m(10) m(11) m(12) m(13)
#define PSIZAM_REPEAT_15(m) PSIZAM_REPEAT_11(m) m(11) m(12) m(13) m(14)
#define PSIZAM_REPEAT_16(m) PSIZAM_REPEAT_12(m) m(12) m(13) m(14) m(15)

#define PSIZAM_ENUM_0(t)
#define PSIZAM_ENUM_1(t) t ## 0
#define PSIZAM_ENUM_2(t) t ## 0, t ## 1
#define PSIZAM_ENUM_3(t) t ## 0, t ## 1, t ## 2
#define PSIZAM_ENUM_4(t) t ## 0, t ## 1, t ## 2, t ## 3
#define PSIZAM_ENUM_5(t) PSIZAM_ENUM_1(t), t ## 1, t ## 2, t ## 3, t ## 4
#define PSIZAM_ENUM_6(t) PSIZAM_ENUM_2(t), t ## 2, t ## 3, t ## 4, t ## 5
#define PSIZAM_ENUM_7(t) PSIZAM_ENUM_3(t), t ## 3, t ## 4, t ## 5, t ## 6
#define PSIZAM_ENUM_8(t) PSIZAM_ENUM_4(t), t ## 4, t ## 5, t ## 6, t ## 7
#define PSIZAM_ENUM_9(t) PSIZAM_ENUM_5(t), t ## 5, t ## 6, t ## 7, t ## 8
#define PSIZAM_ENUM_10(t) PSIZAM_ENUM_6(t), t ## 6, t ## 7, t ## 8, t ## 9
#define PSIZAM_ENUM_11(t) PSIZAM_ENUM_7(t), t ## 7, t ## 8, t ## 9, t ## 10
#define PSIZAM_ENUM_12(t) PSIZAM_ENUM_8(t), t ## 8, t ## 9, t ## 10, t ## 11
#define PSIZAM_ENUM_13(t) PSIZAM_ENUM_9(t), t ## 9, t ## 10, t ## 11, t ## 12
#define PSIZAM_ENUM_14(t) PSIZAM_ENUM_10(t), t ## 10, t ## 11, t ## 12, t ## 13
#define PSIZAM_ENUM_15(t) PSIZAM_ENUM_11(t), t ## 11, t ## 12, t ## 13, t ## 14
#define PSIZAM_ENUM_16(t) PSIZAM_ENUM_12(t), t ## 12, t ## 13, t ## 14, t ## 15
