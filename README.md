# ps4.controller.raw-input.visualizer-mapper

A Windows console application that listens for input from a connected **PlayStation 4 (DualShock 4) controller** via the Windows Raw Input API and:

* Visualizes controller state (sticks, triggers, buttons, D-pad, battery, raw HID bytes) in the console as ASCII art.
* Maps controller input to **mouse** and **keyboard** input using `SendInput`, so you can drive applications with a DualShock 4.
* Provides a compact **virtual keyboard** you can operate with the controller.

The program is intended as a developer / hobby tool for experimenting with controller-to-input mappings and for quickly testing how a PS4 controller can emulate keyboard/mouse input.

---

## Highlights / Quick overview

* **Dual mode:** Visualizer (shows controller state) and Virtual Keyboard (compact, selectable QWERTY layout). Toggle with `TAB` or the controller `OPTIONS` button.
* **Mappings (default):**

  * Left stick → WASD (analog mapped to digital key presses with a deadzone)
  * D-Pad → Arrow keys
  * Right stick → Relative mouse movement (cubic scaling for fine control)
  * R2 → Left mouse button (when pressed past threshold)
  * L2 → Right mouse button (when pressed past threshold)
  * Face buttons → configurable VK mappings (defaults shown below)
* **Virtual keyboard controls:** Left stick to move selection, Cross to press, Square toggles sticky Shift, Circle = Backspace, Triangle = Space.
* **ESC** quits the program. Pressing `v`/`k` on the physical keyboard will also switch to Visualizer/Virtual Keyboard respectively.
* `R1` toggles the console window visibility (show/hide). The console is kept always-on-top.
* `R3` switches IME modes when the virtual keyboard is enabled.

---

## Default mappings

These defaults are created in the program (`initFaceButtonMap()` and related code):

* `SQUARE` → `'E'` (VK `0x45`) — example mapping (edit in code to change)
* `CROSS`  → `Space` (VK `VK_SPACE`)
* `CIRCLE` → `Left Ctrl` (VK `VK_LCONTROL`)
* `TRIANGLE` → `Left Shift` (VK `VK_LSHIFT`)

**Visualizer mode:** face buttons send those mapped keys as press/release events.

**Virtual Keyboard mode:** face buttons are used for keyboard UI actions (regardless of the face-button map):

* `Cross` — press selected virtual key
* `Square` — toggle *sticky* Shift (Shift stays held by the emulator until toggled off)
* `Circle` — Backspace
* `Triangle` — Space

You can change the face button-to-VK mapping by editing the `faceButtonMap` initialization in the source.

---

## Virtual keyboard layout & behaviour

A compact QWERTY-like layout (modifiable in source):

```
Row 0: Q W E R T Y U I O P
Row 1: A S D F G H J K L ENTER
Row 2: Z X C V B N M , . /
Row 3: SPACE BACKSPACE
```

* Use the **left stick** to move the selection. The code implements a small repeat delay (`vkMoveDelayMs`, default 150 ms) so you can hold the stick for continuous movement.
* Press **Cross** to emit the currently selected key via `SendInput`.
* Press **Square** to toggle a sticky Shift state — while sticky Shift is on, subsequent key presses are sent with Shift down. The emulator physically holds and releases `VK_LSHIFT` for you.
* Right stick **still controls the mouse** while in VK mode.

**Note:** The virtual keyboard is intended for simple text entry and testing. It's not a full IME or localized input method; OEM keys and punctuation may differ between keyboard layouts.

---

## Building

Requirements:

* Windows 10 or later (32/64-bit). Raw Input and `SendInput` are Windows APIs.
* Microsoft Visual C++ (MSVC) / Developer Command Prompt, or another C++17-capable compiler that targets Win32.

Open a **Developer Command Prompt for Visual Studio** and run one of the commands below (use the one that fits your toolchain):

```bat
cl /EHsc /std:c++17 main.cpp /link user32.lib
```

or (older MSVC; same effect):

```bat
cl /EHsc main.cpp /link user32.lib
```

This produces `main.exe`.

---

## Running

1. Connect your PS4 DualShock 4 controller via USB or pair it over Bluetooth.
2. Launch the executable from a console window.
3. Move sticks and press buttons — the console updates continuously with a visualization and mapping state.
4. Toggle between **Visualizer** and **Virtual Keyboard** with `TAB` or by pressing the controller `OPTIONS` button. Press `ESC` to exit.
5. Press `R1` to hide/show the console window at any time.

---

## Notable implementation details

* **Raw Input:** the program registers a `RAWINPUTDEVICE` for `UsagePage=0x01` / `Usage=0x05` (Game Pad) with `RIDEV_INPUTSINK` so it receives input while the console does not have to be focused.
* **HID parsing:** the program copies the first HID report into a packed `PS4ControllerReport` structure and uses fields such as `leftStickX`, `buttons1`, `leftTrigger`, `battery`, etc. Report layout (USB vs Bluetooth) can vary slightly across firmware/drivers — adjust the struct if your controller reports a different layout.
* **SendInput:** keyboard and mouse events are generated with `SendInput`. This may be restricted by security or anti-cheat systems; synthetic input can be blocked or flagged by some applications.
* **Mouse movement:** right stick movement is scaled with a cubic curve for finer low-speed control and multiplied by a `sensitivity` constant.
* **Shift sticky:** when sticky Shift is enabled, the program holds `VK_LSHIFT` down until toggled off — this prevents rapid key-up/down behavior for shifted characters.
* **Console window:** the console is set always-on-top on startup. Press `R1` to hide/show it.
* **Key repeat:** `W/A/S/D` and Arrow keys auto-repeat while held (initial 300 ms, then every 70 ms).
* **Mouse event coalescing:** uses `MOUSEEVENTF_MOVE_NOCOALESCE` to improve responsiveness of relative mouse movement.
* **Triggers:** L2 and R2 map to right/left click when pressed past a threshold (default ≈ 50/255).
* **Threading:** a background message thread owns a message-only window and receives Raw Input; the main thread performs mapping and console rendering to keep output single-threaded.

---

## Troubleshooting & known issues

* **Different keyboard layouts:** OEM VK codes for punctuation (`, . / [ ] \ - =`) depend on the physical keyboard layout. If you get unexpected characters from the virtual keyboard, modify `getVkForLabel()` in the source to match your layout.
* **Stuck keys after crash/exit:** the program attempts to release any synthesized keys/buttons on exit. If it terminates abnormally (crash/kill), some keys may remain logically pressed by the OS. Reboot or use a small helper program to send key-up events if needed.
* **Anti-cheat / protected focus applications:** Some games or protected windows ignore synthetic input sent with `SendInput` or may treat it as cheating. Use at your own risk and do not use in online or competitive environments.

---

## Sample console output

```
=== PS4 Controller -> Mouse/Keyboard Mapper ===
Mappings (Visualizer mode):
  Left stick -> WASD (analog -> digital)
  D-Pad -> Arrow keys
  Right stick -> Mouse movement (relative)
  R2 -> Left mouse button, L2 -> Right mouse button
Controls:
Mode: VisualizerTAB to toggle Visualizer/Virtual Keyboard | OPTIONS button toggles too
  In Virtual Keyboard: Left stick to move, Cross(X) to press, Square toggles Shift, Circle Backspace, Triangle Space, L3 JA/EN toggle

Mode: Visualizer
Left Stick:                   Right Stick:                  L2: [..........]   0
...........                   ...........                   R2: [######....] 174
..@........                   ...........
.....+.....                   .....+.....                   Battery:   0
...........                   .........@.
...........                   ...........
X:  52 Y:  61                 X: 235 Y: 203

Buttons:  SQR   CRO  [CIR]  TRI
D-Pad: Neutral
 L1   R1   L3   R3   |  PS   PAD   SHARE   OPTIONS
Raw Data: 01 34 3d eb cb 48 08 58 00 ae 4e c5 00 c7 ff fe ff fb ff 54 03 29 21 12
```

---

## License

This project is released under the **MIT License**. See the `LICENSE.md` file for details.

---

## Where to modify behaviour

Common places to change functionality in the source:

* `initFaceButtonMap()` — change face button → VK mappings.
* `processVisualizerMapping()` / `processVirtualKeyboard()` — change how sticks/triggers/buttons are interpreted.
* `getVkForLabel()` — add or adapt punctuation/OEM mappings for your locale.
* Mouse sensitivity and deadzones are set in `processRightStickMouse()` and can be adjusted there.

