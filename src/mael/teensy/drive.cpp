// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program. If not, see https://www.gnu.org/licenses/.

#ifdef __MK64FX512__

#include "util.hh"
#include "config.hh"
#include "drive.hh"

// Drive speed
static double drv_x;
static double drv_y;
static double drv_w;

// Motor control
static double dc_angle = 0.0;
static int dc_ticks[4] = {};
static int dc_speed[4] = {};

// PID state
static double pid_acc[4] = {};
static double pid_prev[4] = {};

static inline void IncrementEncoderSpeed(int idx, int dir_pin)
{
    dc_ticks[idx] += digitalRead(dir_pin) ? 1 : -1;
}

void InitDrive()
{
    // Encoder speed
    attachInterrupt(digitalPinToInterrupt(ENC0_PIN_INT), []() { IncrementEncoderSpeed(0, ENC0_PIN_DIR); }, FALLING);
    attachInterrupt(digitalPinToInterrupt(ENC1_PIN_INT), []() { IncrementEncoderSpeed(0, ENC1_PIN_DIR); }, FALLING);
    attachInterrupt(digitalPinToInterrupt(ENC2_PIN_INT), []() { IncrementEncoderSpeed(0, ENC2_PIN_DIR); }, FALLING);
    attachInterrupt(digitalPinToInterrupt(ENC3_PIN_INT), []() { IncrementEncoderSpeed(0, ENC3_PIN_DIR); }, FALLING);

    // DC driver direction pins
    pinMode(DC0_PIN_DIR, OUTPUT);
    pinMode(DC1_PIN_DIR, OUTPUT);
    pinMode(DC2_PIN_DIR, OUTPUT);
    pinMode(DC3_PIN_DIR, OUTPUT);

    // DC driver PWM pins
    pinMode(DC0_PIN_PWM, OUTPUT);
    pinMode(DC1_PIN_PWM, OUTPUT);
    pinMode(DC2_PIN_PWM, OUTPUT);
    pinMode(DC3_PIN_PWM, OUTPUT);
}

static void WriteMotorSpeed(int dir_pin, int pwm_pin, int speed)
{
    if (speed >= 0) {
        digitalWrite(dir_pin, 0);
        analogWrite(pwm_pin, speed);
    } else {
        digitalWrite(dir_pin, 1);
        analogWrite(pwm_pin, -speed);
    }
}

void ProcessDrive()
{
    PROCESS_EVERY(5000);

    // Forward kinematics matrix:
    // -sin((45 + 90)°)  | cos((45 + 90)°)  | 1
    // -sin((135 + 90)°) | cos((135 + 90)°) | 1
    // -sin((225 + 90)°) | cos((225 + 90)°) | 1
    // -sin((315 + 90)°) | cos((315 + 90)°) | 1
    //
    // Inverse kinematics matrix:
    // -1/sqrt(2) | -1/sqrt(2) | 1
    //  1/sqrt(2) | -1/sqrt(2) | 1
    //  1/sqrt(2) |  1/sqrt(2) | 1
    // -1/sqrt(2) |  1/sqrt(2) | 1

    // DC speed constants
    static const double kl = 1.0;
    static const double kw = 1.0;

    // PID constants
    static const double kp = 1.0;
    static const double ki = 0.0;
    static const double kd = 0.0;

    int ticks[4];
    noInterrupts();
    memcpy(ticks, dc_ticks, sizeof(dc_ticks));
    memset(dc_ticks, 0, sizeof(dc_ticks));
    interrupts();

    // Eventually we will integrate gyroscope information (Kalman filter)
    dc_angle += (double)ticks[0] / kw + (double)ticks[1] / kw +
                (double)ticks[2] / kw + (double)ticks[3] / kw;

    // World coordinates to robot coordinates
    double self_x = drv_x * cosf(-dc_angle) - drv_y * sinf(-dc_angle);
    double self_y = drv_x * sinf(-dc_angle) + drv_y * cosf(-dc_angle);
    double self_w = drv_w;

    // Compute target speed for all 4 motors
    int target[4] = {};
    {
        double x = self_x * kl;
        double y = self_y * kl;
        double w = self_w * kw;

        target[0] = (int)(x * -0.7071 + y * -0.7071 + w * 1.0);
        target[1] = (int)(x *  0.7071 + y * -0.7071 + w * 1.0);
        target[2] = (int)(x *  0.7071 + y *  0.7071 + w * 1.0);
        target[3] = (int)(x * -0.7071 + y *  0.7071 + w * 1.0);
    }

    // Run target DC speeds through PID controller
    for (int i = 0; i < 4; i++) {
        double error = (double)(target[i] - ticks[i]);
        double delta = error - pid_prev[i];

        ticks[i] = 0;
        pid_acc[i] += error;
        pid_prev[i] = error;

        dc_speed[i] += (int)(kp * error + ki * pid_acc[i] + kd * delta);
        dc_speed[i] = constrain(dc_speed[i], -255, 255);
    }

    WriteMotorSpeed(DC0_PIN_DIR, DC0_PIN_PWM, dc_speed[0]);
    WriteMotorSpeed(DC1_PIN_DIR, DC1_PIN_PWM, dc_speed[1]);
    WriteMotorSpeed(DC2_PIN_DIR, DC2_PIN_PWM, dc_speed[2]);
    WriteMotorSpeed(DC3_PIN_DIR, DC3_PIN_PWM, dc_speed[3]);
}

void SetDriveSpeed(double x, double y, double w)
{
    drv_x = x;
    drv_y = y;
    drv_w = fmodf(-w, 360.0) * PI / 180.0;
}

#endif
