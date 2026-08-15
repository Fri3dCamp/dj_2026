var Fri3dDJ = {};

// Wheel sends an absolute encoder position, 0..127 wrapping, one CC per detent.
// The "active" CC (0x45/0x55) is not a touch sensor: it pulses 127 before and 0
// after every burst of position messages, so scratch is released by an
// inactivity timer instead of by that CC.
Fri3dDJ.wheelResolution = 128; // detents per revolution
Fri3dDJ.rpm = 33 + 1 / 3;
Fri3dDJ.alpha = 1.0 / 8;
Fri3dDJ.beta = (1.0 / 8) / 32;
Fri3dDJ.releaseAfterMs = 100;

Fri3dDJ.lastPosition = {};
Fri3dDJ.releaseTimer = {};

// Button LEDs: CC 0x20..0x27 (same physical order as button CCs 0x60..0x67),
// value selects a firmware palette color.
Fri3dDJ.color = {
    off: 0,
    orangeRed: 1,
    teal: 2,
    yellowGreen: 3,
    warmWhite: 4,
    blue: 5,
    cyan: 6,
    white: 7,
    brightWhite: 8,
    green: 9,
};

Fri3dDJ.leds = [
    { cc: 0x20, group: "[Channel1]", control: "cue_indicator", color: 1 },
    { cc: 0x21, group: "[Channel1]", control: "hotcue_1_status", color: 5 },
    { cc: 0x22, group: "[Channel2]", control: "hotcue_1_status", color: 5 },
    { cc: 0x23, group: "[Channel2]", control: "cue_indicator", color: 1 },
    { cc: 0x24, group: "[Channel1]", control: "play_indicator", color: 9 },
    { cc: 0x25, group: "[Channel1]", control: "hotcue_2_status", color: 6 },
    { cc: 0x26, group: "[Channel2]", control: "hotcue_2_status", color: 6 },
    { cc: 0x27, group: "[Channel2]", control: "play_indicator", color: 9 },
];

Fri3dDJ.connections = [];

Fri3dDJ.init = function (_id, _debugging) {
    Fri3dDJ.leds.forEach(function (led) {
        var conn = engine.makeConnection(led.group, led.control, function (value) {
            midi.sendShortMsg(
                0xb0,
                led.cc,
                value > 0 ? led.color : Fri3dDJ.color.off
            );
        });
        Fri3dDJ.connections.push(conn);
        conn.trigger();
    });
};

Fri3dDJ.shutdown = function () {
    engine.scratchDisable(1);
    engine.scratchDisable(2);
    Fri3dDJ.connections.forEach(function (conn) {
        conn.disconnect();
    });
    Fri3dDJ.connections = [];
    Fri3dDJ.leds.forEach(function (led) {
        midi.sendShortMsg(0xb0, led.cc, Fri3dDJ.color.off);
    });
};

Fri3dDJ.deckFromGroup = function (group) {
    return parseInt(group.substring(8, 9), 10);
};

Fri3dDJ.engage = function (deck) {
    if (!engine.isScratching(deck)) {
        Fri3dDJ.lastPosition[deck] = null;
        engine.scratchEnable(
            deck,
            Fri3dDJ.wheelResolution,
            Fri3dDJ.rpm,
            Fri3dDJ.alpha,
            Fri3dDJ.beta
        );
    }
};

Fri3dDJ.armRelease = function (deck) {
    if (Fri3dDJ.releaseTimer[deck]) {
        engine.stopTimer(Fri3dDJ.releaseTimer[deck]);
    }
    Fri3dDJ.releaseTimer[deck] = engine.beginTimer(
        Fri3dDJ.releaseAfterMs,
        function () {
            Fri3dDJ.releaseTimer[deck] = null;
            Fri3dDJ.lastPosition[deck] = null;
            engine.scratchDisable(deck, true);
        },
        true
    );
};

// The firmware emits duplicate button messages: a press can repeat 127 a few
// times (also spuriously mid-hold) and a release can repeat 0. Only act on
// real state transitions.
Fri3dDJ.buttonState = {};

Fri3dDJ.buttonEdge = function (control, value) {
    var pressed = value > 0;
    if (Fri3dDJ.buttonState[control] === pressed) {
        return null;
    }
    Fri3dDJ.buttonState[control] = pressed;
    return pressed;
};

Fri3dDJ.playButton = function (_channel, control, value, _status, group) {
    if (Fri3dDJ.buttonEdge(control, value) === true) {
        script.toggleControl(group, "play");
    }
};

// Pressing cue while playing must only stop at the cue point; preview may
// not start until the button is released and pressed again. cue_default
// would flow from the stop straight into preview while held, so the playing
// case goes through cue_gotoandstop and its release is swallowed.
Fri3dDJ.cueSuppressRelease = {};

Fri3dDJ.cueButton = function (_channel, control, value, _status, group) {
    var edge = Fri3dDJ.buttonEdge(control, value);
    if (edge === null) {
        return;
    }
    if (edge) {
        if (engine.getValue(group, "play") > 0) {
            Fri3dDJ.cueSuppressRelease[control] = true;
            engine.setValue(group, "cue_gotoandstop", 1);
            engine.setValue(group, "cue_gotoandstop", 0);
        } else {
            Fri3dDJ.cueSuppressRelease[control] = false;
            engine.setValue(group, "cue_default", 1);
        }
    } else if (!Fri3dDJ.cueSuppressRelease[control]) {
        engine.setValue(group, "cue_default", 0);
    }
};

// Short press: activate (set / jump). Held past the threshold: clear.
Fri3dDJ.longPressMs = 500;
Fri3dDJ.hotcueForControl = { 0x61: 1, 0x62: 1, 0x65: 2, 0x66: 2 };
Fri3dDJ.hotcueTimer = {};

Fri3dDJ.hotcueButton = function (_channel, control, value, _status, group) {
    var num = Fri3dDJ.hotcueForControl[control];
    var edge = Fri3dDJ.buttonEdge(control, value);
    if (edge === null) {
        return;
    }
    if (edge) {
        Fri3dDJ.hotcueTimer[control] = engine.beginTimer(
            Fri3dDJ.longPressMs,
            function () {
                Fri3dDJ.hotcueTimer[control] = null;
                engine.setValue(group, "hotcue_" + num + "_clear", 1);
                engine.setValue(group, "hotcue_" + num + "_clear", 0);
            },
            true
        );
    } else if (Fri3dDJ.hotcueTimer[control]) {
        engine.stopTimer(Fri3dDJ.hotcueTimer[control]);
        Fri3dDJ.hotcueTimer[control] = null;
        engine.setValue(group, "hotcue_" + num + "_activate", 1);
        engine.setValue(group, "hotcue_" + num + "_activate", 0);
    }
};

// Pulses 127 just before a burst of position messages: use it to engage
// scratch early so the first detent already ticks. The 0 pulse is ignored;
// release is handled by the inactivity timer.
Fri3dDJ.wheelTouch = function (_channel, _control, value, _status, group) {
    if (value > 0) {
        Fri3dDJ.engage(Fri3dDJ.deckFromGroup(group));
    }
};

Fri3dDJ.wheelTurn = function (_channel, _control, value, _status, group) {
    var deck = Fri3dDJ.deckFromGroup(group);
    Fri3dDJ.engage(deck);
    var last = Fri3dDJ.lastPosition[deck];
    Fri3dDJ.lastPosition[deck] = value;
    Fri3dDJ.armRelease(deck);
    if (last === null || last === undefined) {
        return;
    }
    // Shortest signed distance on the 0..127 ring; the encoder counts
    // opposite to Mixxx's forward direction, hence the negation.
    var delta = -(((value - last + 192) % 128) - 64);
    engine.scratchTick(deck, delta);
};
