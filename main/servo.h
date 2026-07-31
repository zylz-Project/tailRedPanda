#pragma once

enum ServoIndex {
    SERVO_HEAD   = 0,
    SERVO_TAIL_LR = 1,
    SERVO_TAIL_UD = 2,
};

void InitServos();
void SetServoAngle(int idx, int angle);

extern const int kServoCount;
