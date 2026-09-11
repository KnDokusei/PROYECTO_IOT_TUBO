import { IsBoolean, IsNotEmpty, IsNumber, IsObject, IsOptional } from "class-validator";
import { DeviceMQTT } from "./exp_base";

/*
 * Kundt tube. Unlike the other experiments, this one is driven by three
 * independent ESP32 modules that never talk to each other:
 *
 *   E1-Mic       -> mic_rms, mic_peak
 *   E2-SineGen   -> frequency, volume
 *   E3-StepMotor -> plunger_pos, plunger_actual, clockwise_limit, counter_limit
 *
 * They all publish to the same topic and each one sends only its own fields,
 * so every member below is optional and the listener merges what it receives.
 * This is the reason the `Sensors` class is not `@IsNotEmpty()` like the ones
 * belonging to single-controller experiments.
 */

export class KundtActuators {
    // E2-SineGen: AD9833 output frequency, in Hz
    @IsOptional()
    @IsNumber()
    frequency: number;

    // E2-SineGen: servo angle driving the volume potentiometer, in degrees.
    // Not a percentage, despite the name used by the original firmware.
    @IsOptional()
    @IsNumber()
    volume: number;

    // E3-StepMotor: requested plunger position, in cm from the speaker end
    @IsOptional()
    @IsNumber()
    plunger_pos: number;

    /*
     * E3-StepMotor: one-shot request to re-run the limit-switch homing routine.
     *
     * Unlike the fields above this one is NOT persisted: it is a command, not a
     * setpoint. Storing it would make every later update re-trigger homing, and
     * the column would never describe the rig's actual state. `setKundtExp`
     * forwards it over MQTT and drops it before writing the row.
     */
    @IsOptional()
    @IsBoolean()
    calibrate?: boolean;
}

export class KundtSensors {
    // E1-Mic: Goertzel magnitude at the frequency E2 is driving. This is the
    // measurement: a single DFT bin rejects the stepper, the fans and the
    // supply ripple that broadband RMS would fold into the reading.
    @IsOptional()
    @IsNumber()
    mic_amplitude: number;

    // E1-Mic: broadband RMS of the last window. Diagnostic only: comparing it
    // against mic_amplitude tells how much of the level is actually the tone.
    @IsOptional()
    @IsNumber()
    mic_rms: number;

    // E1-Mic: peak sample of the last window. Saturation detector: a value
    // pinned at full scale means the front end is clipping.
    @IsOptional()
    @IsNumber()
    mic_peak: number;

    // E3-StepMotor: position actually reached, in cm
    @IsOptional()
    @IsNumber()
    plunger_actual: number;

    @IsOptional()
    @IsBoolean()
    clockwise_limit: boolean;

    @IsOptional()
    @IsBoolean()
    counter_limit: boolean;
}

export class KundtRow {
    id: number;
    frequency: number;
    volume: number;
    plunger_pos: number;
    plunger_actual: number;
    clockwise_limit: boolean;
    counter_limit: boolean;
    mic_amplitude: number;
    mic_rms: number;
    mic_peak: number;
}

export class KundtInfo {
    // Usable rail travel, in cm. Reported by E3 after homing.
    @IsNumber()
    @IsNotEmpty()
    limit_2_limit: number;
}

export class KundtMQTT {
    @IsObject()
    @IsNotEmpty()
    device: DeviceMQTT;

    @IsOptional()
    @IsObject()
    actuators: KundtActuators;

    @IsOptional()
    @IsObject()
    sensors: KundtSensors;

    @IsOptional()
    @IsObject()
    info: KundtInfo;
}
