# FG2009 MCU bring-up

This firmware profile supports relative Focus/Zoom motion and the two-wire
IR-CUT actuator. FG2009 has no PI/home feedback, so absolute motion, homing,
PI reads, and iris operations intentionally return `SYS_ERR_NOT_SUPPORTED`.

## Build

AF0832 and FG2009 are compiled into one universal firmware. AF0832 remains
the power-on default for compatibility:

```text
make -C mcu_board_prj app-build GCC_PATH=<cubeide-gcc-bin>
```

Select FG2009 at runtime before lens initialization:

```text
lens profile set fg2009
lens init
lens cfg motor
```

The selection is not persisted in MCU flash. Repeat it after every MCU reset.

The confirmed open-loop coordinate limits from the vendor focus curve are
Zoom TELE = 2463 physical steps and Focus FAR = 2453 physical steps. These
values describe the software travel coordinates; FG2009 still has no PI or
stall feedback, so reaching a mechanical end remains an assumed hard-stop
initialization rather than a sensed home operation.

## Wiring assumptions

- J9 / MS41908M AB channel drives Focus.
- J9 / MS41908M CD channel drives Zoom.
- FG2009 Zoom pins are connected 1:1 to J9.
- FG2009 Focus pins 3/4 are connected 1:1; the profile compensates
  their reversed A/A-bar order with `focus.direction_sign = -1`.
- J7 drives the FG2009 Blue/Black IR-CUT pair through AP1511B.

Do not both swap Focus pins 3/4 and keep the software inversion.

## Safe first motion

Measure each coil pair before power-on; the expected resistance is about
40 ohms. Put both moving groups near mid-travel. Do not start at a mechanical
end stop.

```text
lens init
lens cfg motor
lens profile
lens caps

lens focus rel 500 10
lens focus wait 5000
lens focus rel 500 -10
lens focus wait 5000

lens zoom rel 500 10
lens zoom wait 5000
lens zoom rel 500 -10
lens zoom wait 5000
```

For FG2009, `500 pps / 10 physical steps` is logged and converted to
`4000 raw pps / 80 raw PSUM units`. Focus raw direction is inverted by the
profile.

Bench verification established these physical directions:

- positive Zoom steps: Tele to Wide;
- negative Zoom steps: Wide to Tele;
- positive Focus steps: Far to Near;
- negative Focus steps: Near to Far.

After direction and current are confirmed, repeat at 600, 700, 800, and
finally 900 physical pps. Coil peak current must remain below 145 mA.

## Dual relative motion

```text
lens dual rel 500 10 500 10
lens dual wait 5000
```

The existing `run`, `raw`, and Host Link relative commands keep their legacy
MS41908M raw-unit semantics.

## IR-CUT

```text
lens ircut state
lens ircut day
# wait at least two seconds
lens ircut night
```

Bench verification established Day=PB8 low and Night=PB8 high: low inserts
the IR-cut filter and high removes it. The controller suppresses duplicate
requests and rejects a reverse transition within two seconds. It does not
toggle PB8 back after 100 ms because that could command the opposite IR-CUT
direction.

## Expected unsupported commands

The following commands must return `SYS_ERR_NOT_SUPPORTED` in the FG2009
build:

```text
lens zoom abs ...
lens focus abs ...
lens zoom rz ...
lens focus rz ...
lens pi
lens iris ...
```

Reported positions are relative diagnostic counters only and are not valid
absolute optical positions after power-up or interruption.
