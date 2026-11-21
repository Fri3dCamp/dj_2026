var Fri3dDJ = {};

// Decks 1 and 2 use MIDI channel 1, decks 3 and 4 the same CC numbers on
// channel 2: a Fri3d badge running dj_app switches each side of the add-on
// between deck 1/3 (left) and 2/4 (right). The crossfader is on channel 1.

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
// value selects a firmware palette color. Decks 3 and 4 use channel 2.
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

Fri3dDJ.ledLayout = [
    { cc: 0x20, side: 0, control: "cue_indicator", color: 1 },
    { cc: 0x21, side: 0, control: "hotcue_1_status", color: 5 },
    { cc: 0x22, side: 1, control: "hotcue_1_status", color: 5 },
    { cc: 0x23, side: 1, control: "cue_indicator", color: 1 },
    { cc: 0x24, side: 0, control: "play_indicator", color: 9 },
    { cc: 0x25, side: 0, control: "hotcue_2_status", color: 6 },
    { cc: 0x26, side: 1, control: "hotcue_2_status", color: 6 },
    { cc: 0x27, side: 1, control: "play_indicator", color: 9 },
];

// The LEDs of decks 1 and 2 on channel 1 (0xB0), of decks 3 and 4 on channel 2 (0xB1)
Fri3dDJ.leds = [];
[0, 1].forEach(function (layer) {
    Fri3dDJ.ledLayout.forEach(function (led) {
        Fri3dDJ.leds.push({
            status: 0xb0 + layer,
            cc: led.cc,
            group: "[Channel" + (1 + led.side + 2 * layer) + "]",
            control: led.control,
            color: led.color,
        });
    });
});

Fri3dDJ.connections = [];

Fri3dDJ.init = function (_id, _debugging) {
    Fri3dDJ.leds.forEach(function (led) {
        var conn = engine.makeConnection(led.group, led.control, function (value) {
            midi.sendShortMsg(
                led.status,
                led.cc,
                value > 0 ? led.color : Fri3dDJ.color.off
            );
        });
        Fri3dDJ.connections.push(conn);
        conn.trigger();
    });
};

Fri3dDJ.shutdown = function () {
    [1, 2, 3, 4].forEach(function (deck) {
        engine.scratchDisable(deck);
    });
    Fri3dDJ.connections.forEach(function (conn) {
        conn.disconnect();
    });
    Fri3dDJ.connections = [];
    Fri3dDJ.leds.forEach(function (led) {
        midi.sendShortMsg(led.status, led.cc, Fri3dDJ.color.off);
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
// real state transitions. A button drives deck 1 or 3 (2 or 4) depending on
// the channel, so the state is per channel and control.
Fri3dDJ.buttonState = {};

Fri3dDJ.buttonKey = function (channel, control) {
    return channel * 256 + control;
};

Fri3dDJ.buttonEdge = function (channel, control, value) {
    var key = Fri3dDJ.buttonKey(channel, control);
    var pressed = value > 0;
    if (Fri3dDJ.buttonState[key] === pressed) {
        return null;
    }
    Fri3dDJ.buttonState[key] = pressed;
    return pressed;
};

Fri3dDJ.playButton = function (channel, control, value, _status, group) {
    if (Fri3dDJ.buttonEdge(channel, control, value) === true) {
        script.toggleControl(group, "play");
    }
};

// Pressing cue while playing must only stop at the cue point; preview may
// not start until the button is released and pressed again. cue_default
// would flow from the stop straight into preview while held, so the playing
// case goes through cue_gotoandstop and its release is swallowed.
Fri3dDJ.cueSuppressRelease = {};

Fri3dDJ.cueButton = function (channel, control, value, _status, group) {
    var key = Fri3dDJ.buttonKey(channel, control);
    var edge = Fri3dDJ.buttonEdge(channel, control, value);
    if (edge === null) {
        return;
    }
    if (edge) {
        if (engine.getValue(group, "play") > 0) {
            Fri3dDJ.cueSuppressRelease[key] = true;
            engine.setValue(group, "cue_gotoandstop", 1);
            engine.setValue(group, "cue_gotoandstop", 0);
        } else {
            Fri3dDJ.cueSuppressRelease[key] = false;
            engine.setValue(group, "cue_default", 1);
        }
    } else if (!Fri3dDJ.cueSuppressRelease[key]) {
        engine.setValue(group, "cue_default", 0);
    }
};

// Short press: activate (set / jump). Held past the threshold: clear.
Fri3dDJ.longPressMs = 500;
Fri3dDJ.hotcueForControl = { 0x61: 1, 0x62: 1, 0x65: 2, 0x66: 2 };
Fri3dDJ.hotcueTimer = {};

Fri3dDJ.hotcueButton = function (channel, control, value, _status, group) {
    var key = Fri3dDJ.buttonKey(channel, control);
    var num = Fri3dDJ.hotcueForControl[control];
    var edge = Fri3dDJ.buttonEdge(channel, control, value);
    if (edge === null) {
        return;
    }
    if (edge) {
        Fri3dDJ.hotcueTimer[key] = engine.beginTimer(
            Fri3dDJ.longPressMs,
            function () {
                Fri3dDJ.hotcueTimer[key] = null;
                engine.setValue(group, "hotcue_" + num + "_clear", 1);
                engine.setValue(group, "hotcue_" + num + "_clear", 0);
            },
            true
        );
    } else if (Fri3dDJ.hotcueTimer[key]) {
        engine.stopTimer(Fri3dDJ.hotcueTimer[key]);
        Fri3dDJ.hotcueTimer[key] = null;
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
