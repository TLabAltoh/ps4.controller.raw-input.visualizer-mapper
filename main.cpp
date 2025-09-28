#include <windows.h>
#include <iostream>
#include <iomanip>
#include <vector>
#include <array>
#include <mutex>
#include <optional>
#include <thread>
#include <chrono>
#include <conio.h>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <cmath>
#include <map>
#include <algorithm>
#include <cctype>

#pragma pack(push, 1)
struct PS4ControllerReport {
    uint8_t reportId;
    uint8_t leftStickX;
    uint8_t leftStickY;
    uint8_t rightStickX;
    uint8_t rightStickY;
    uint8_t buttons1;      // D-pad (lower nibble) and face buttons (upper nibble)
    uint8_t buttons2;      // Shoulder buttons and stick clicks
    uint8_t buttons3;      // PS, touchpad, share, options (approx.)
    uint8_t leftTrigger;
    uint8_t rightTrigger;
    uint8_t unknown1[2];
    uint8_t gyroX[2];
    uint8_t gyroY[2];
    uint8_t gyroZ[2];
    uint8_t accelX[2];
    uint8_t accelY[2];
    uint8_t accelZ[2];
    uint8_t unknown2[5];
    uint8_t battery;
    uint8_t unknown3[4];
    uint8_t touchpad[3];
    uint8_t unknown4[21];
};
#pragma pack(pop)

// ---------- Console helper ----------
class Console {
public:
    Console() : hOut(GetStdHandle(STD_OUTPUT_HANDLE)) {
        if (hOut == INVALID_HANDLE_VALUE) throw std::runtime_error("Failed to get console output handle");
        CONSOLE_CURSOR_INFO info {};
        if (GetConsoleCursorInfo(hOut, &info)) savedCursorInfo = info;
        hideCursor();
    }
    ~Console() {
        if (hOut != INVALID_HANDLE_VALUE) SetConsoleCursorInfo(hOut, &savedCursorInfo);
    }
    void clear() {
        COORD topLeft {0, 0};
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (!GetConsoleScreenBufferInfo(hOut, &csbi)) return;
        DWORD len = static_cast<DWORD>(csbi.dwSize.X) * csbi.dwSize.Y;
        DWORD written = 0;
        FillConsoleOutputCharacterA(hOut, ' ', len, topLeft, &written);
        FillConsoleOutputAttribute(hOut, csbi.wAttributes, len, topLeft, &written);
        SetConsoleCursorPosition(hOut, topLeft);
    }
    void setCursor(int x, int y) {
        COORD coord { static_cast<SHORT>(x), static_cast<SHORT>(y) };
        SetConsoleCursorPosition(hOut, coord);
    }
    void hideCursor() {
        CONSOLE_CURSOR_INFO info {};
        info.bVisible = FALSE;
        info.dwSize = 1;
        SetConsoleCursorInfo(hOut, &info);
    }
    void writeAt(int x, int y, const std::string& s) {
        setCursor(x, y);
        std::cout << s;
    }
    static std::string bytesToHex(const uint8_t* data, size_t len) {
        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (size_t i = 0; i < len; ++i) {
            ss << std::setw(2) << static_cast<int>(data[i]) << ' ';
        }
        ss << std::dec;
        return ss.str();
    }
private:
    HANDLE hOut;
    CONSOLE_CURSOR_INFO savedCursorInfo {};
};

// ---------- Input emulation helpers (mouse + keyboard) ----------
namespace Emu {
    // send keyboard key down/up using SendInput
    void sendKey(WORD vk, bool down) {
        INPUT in{};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        SendInput(1, &in, sizeof(in));
    }

    // send mouse relative movement
    void sendMouseMoveRelative(int dx, int dy) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = dx;
        in.mi.dy = dy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;
        SendInput(1, &in, sizeof(in));
    }

    void sendMouseButton(bool left, bool down) {
        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = left ? (down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP)
                             : (down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
        SendInput(1, &in, sizeof(in));
    }
}

// ---------- PS4 Visualizer + Mapper + Virtual Keyboard ----------
class PS4VisualizerMapper {
public:
    PS4VisualizerMapper()
    {
        registerWindowClass();
        createMessageWindow();
        registerRawInput();
        console.clear();
        initFaceButtonMap();
        initVirtualKeyboard();
        printHeader();
    }

    ~PS4VisualizerMapper() {
        RAWINPUTDEVICE removeRid{};
        removeRid.usUsagePage = 0x01;
        removeRid.usUsage = 0x05;
        removeRid.dwFlags = RIDEV_REMOVE;
        removeRid.hwndTarget = nullptr;
        RegisterRawInputDevices(&removeRid, 1, sizeof(removeRid));

        // ensure any held inputs are released
        releaseAllInputs();

        if (hwnd) DestroyWindow(hwnd);
        UnregisterClassW(windowClassName.c_str(), GetModuleHandle(nullptr));
    }

    void run() {
        MSG msg;
        bool done = false;
        while (!done) {
            if (_kbhit()) {
                int ch = _getch();
                // Check for special key prefix
                if (ch == 0 || ch == 0xE0) {
                    int special = _getch(); // fetch actual code
                } else {
                    if (ch == 27) { // ESC
                        done = true;
                        break;
                    } else if (ch == 9) { // TAB -> toggle mode
                        toggleMode();
                    } else if (ch == 'v' || ch == 'V') {
                        // explicit visualizer request
                        setMode(MODE_VISUALIZER);
                    } else if (ch == 'k' || ch == 'K') {
                        // explicit keyboard request
                        setMode(MODE_VKEYBOARD);
                    }
                }
            }
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            // handle repeats for WASD and arrow keys
            handleKeyRepeats();

            // update display occasionally (we update on input too)
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
        // on exit, release any held keys/buttons
        releaseAllInputs();
    }

private:
    // Window / raw input setup
    void registerWindowClass() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = windowClassName.c_str();
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

        if (!RegisterClassW(&wc)) {
            DWORD err = GetLastError();
            if (err != ERROR_CLASS_ALREADY_EXISTS) {
                throw std::runtime_error("RegisterClass failed: " + std::to_string(err));
            }
        }
    }

    void createMessageWindow() {
        hwnd = CreateWindowW(windowClassName.c_str(), L"PS4RawInputHiddenWindow",
                             0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandle(nullptr), this);
        if (!hwnd) throw std::runtime_error("CreateWindow failed");
    }

    void registerRawInput() {
        RAWINPUTDEVICE rid{};
        rid.usUsagePage = 0x01; // Generic Desktop
        rid.usUsage = 0x05;     // Game Pad
        rid.dwFlags = RIDEV_INPUTSINK;
        rid.hwndTarget = hwnd;

        if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
            DWORD err = GetLastError();
            throw std::runtime_error("RegisterRawInputDevices failed: " + std::to_string(err));
        }
    }

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        PS4VisualizerMapper* self = nullptr;
        if (msg == WM_CREATE) {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<PS4VisualizerMapper*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<PS4VisualizerMapper*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (self) return self->handleMessage(msg, wParam, lParam);
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    LRESULT handleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
        switch (msg) {
            case WM_INPUT:
                handleRawInput(reinterpret_cast<HRAWINPUT>(lParam));
                return 0;
            default:
                return DefWindowProc(hwnd, msg, wParam, lParam);
        }
    }

    void handleRawInput(HRAWINPUT hRaw) {
        UINT size = 0;
        if (GetRawInputData(hRaw, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) return;
        if (size == 0) return;

        std::vector<BYTE> buffer(size);
        if (GetRawInputData(hRaw, RID_INPUT, buffer.data(), &size, sizeof(RAWINPUTHEADER)) != size) return;

        RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(buffer.data());
        if (raw->header.dwType != RIM_TYPEHID) return;

        if (raw->data.hid.dwSizeHid >= sizeof(PS4ControllerReport) && raw->data.hid.dwCount >= 1) {
            PS4ControllerReport report{};
            std::memcpy(&report, raw->data.hid.bRawData, sizeof(report));

            {
                std::lock_guard<std::mutex> lk(stateMutex);
                lastReport = report;
                controllerConnected = true;
            }

            // process mapping immediately (so input emulation is reactive)
            processMapping(report);
            // update UI
            updateDisplay();
        }
    }

    // ---------- Mapping logic ----------
    static float normalizeAxis(uint8_t v) {
        // center roughly 128 -> map to [-1, +1]
        return (static_cast<int>(v) - 128) / 127.0f;
    }

    // helper constants for WASD virtual keys
    static constexpr WORD VK_KEY_W = 0x57; // 'W'
    static constexpr WORD VK_KEY_A = 0x41; // 'A'
    static constexpr WORD VK_KEY_S = 0x53; // 'S'
    static constexpr WORD VK_KEY_D = 0x44; // 'D'

    // modes
    enum Mode {
        MODE_VISUALIZER = 0,
        MODE_VKEYBOARD  = 1
    };

    // ---------- Mapping state ----------
    void initFaceButtonMap() {
        faceButtonMap = {
            {"SQUARE", 'E'},     // Example: Square -> 'E'
            {"CROSS", VK_SPACE}, // Cross -> Space
            {"CIRCLE", VK_LCONTROL}, // Circle -> Left Ctrl
            {"TRIANGLE", VK_LSHIFT}  // Triangle -> Left Shift
        };
        // initialize faceButtonState and controllerPrev
        faceButtonState.clear();
        controllerPrev.clear();
        faceButtonState["SQUARE"] = false;
        faceButtonState["CROSS"] = false;
        faceButtonState["CIRCLE"] = false;
        faceButtonState["TRIANGLE"] = false;
        controllerPrev["SQUARE"] = false;
        controllerPrev["CROSS"] = false;
        controllerPrev["CIRCLE"] = false;
        controllerPrev["TRIANGLE"] = false;
        prevOptions = false;
    }

    void initVirtualKeyboard() {
        // Simple QWERTY-like compact layout
        vkLayout = {
            {"Q","W","E","R","T","Y","U","I","O","P"},
            {"A","S","D","F","G","H","J","K","L","ENTER"},
            {"Z","X","C","V","B","N","M",",",".","/"},
            {"SPACE","BACKSPACE"}
        };
        vkRows = static_cast<int>(vkLayout.size());
        selRow = 0;
        selCol = 0;
        vkMoveDelayMs = 150; // ms between selection moves while holding stick
        lastVKMove = std::chrono::steady_clock::now();
        shiftSticky = false;
        shiftHeldByEmulator = false;
        mode = MODE_VISUALIZER;
    }

    // Helper to update a mapped face button (used in visualizer mode)
    void handleFaceButton(const std::string& name, bool pressed) {
        WORD vk = faceButtonMap[name];
        bool currentlyDown = faceButtonState[name];

        if (pressed && !currentlyDown) {
            Emu::sendKey(vk, true);
            faceButtonState[name] = true;
        } else if (!pressed && currentlyDown) {
            Emu::sendKey(vk, false);
            faceButtonState[name] = false;
        }
    }

    void processMapping(const PS4ControllerReport& r) {
        // OPTIONS button toggles mode on rising edge
        bool optionsPressed = (r.buttons2 & 0x20) != 0;
        if (optionsPressed && !prevOptions) {
            toggleMode();
        }
        prevOptions = optionsPressed;
        // If in keyboard mode, handle virtual keyboard mapping
        if (mode == MODE_VKEYBOARD) {
            processVirtualKeyboard(r);
        } else {
            processVisualizerMapping(r);
        }
        // Right stick -> mouse remains available in both modes
        processRightStickMouse(r);
    }

    void processVisualizerMapping(const PS4ControllerReport& r) {
        // left stick to WASD
        const float deadzone = 0.25f; // axis below this = not pressed
        float lx = normalizeAxis(r.leftStickX);
        float ly = normalizeAxis(r.leftStickY);
        ly = -ly; // conventional forward mapping (stick up -> positive)

        bool wantW = (ly > deadzone);
        bool wantS = (ly < -deadzone);
        bool wantA = (lx < -deadzone);
        bool wantD = (lx > deadzone);

        setKeyState(VK_KEY_W, wantW);
        setKeyState(VK_KEY_S, wantS);
        setKeyState(VK_KEY_A, wantA);
        setKeyState(VK_KEY_D, wantD);

        // D-Pad -> arrow keys
        uint8_t dpad = r.buttons1 & 0x0F;
        bool up = false, down = false, left = false, right = false;
        switch (dpad) {
            case 0: up = true; break;
            case 1: up = true; right = true; break;
            case 2: right = true; break;
            case 3: right = true; down = true; break;
            case 4: down = true; break;
            case 5: down = true; left = true; break;
            case 6: left = true; break;
            case 7: left = true; up = true; break;
            default: break; // neutral
        }
        setKeyState(VK_UP, up);
        setKeyState(VK_DOWN, down);
        setKeyState(VK_LEFT, left);
        setKeyState(VK_RIGHT, right);

        // Triggers as mouse buttons: R2 -> left click, L2 -> right click
        const uint8_t pressThreshold = 50; // 0..255
        bool wantLeftClick = r.rightTrigger > pressThreshold;
        bool wantRightClick = r.leftTrigger > pressThreshold;
        setMouseButtonState(true, wantLeftClick);  // left
        setMouseButtonState(false, wantRightClick); // right

        // Face buttons mapping
        handleFaceButton("SQUARE",   (r.buttons1 & 0x10) != 0);
        handleFaceButton("CROSS",    (r.buttons1 & 0x20) != 0);
        handleFaceButton("CIRCLE",   (r.buttons1 & 0x40) != 0);
        handleFaceButton("TRIANGLE", (r.buttons1 & 0x80) != 0);
    }

    void processRightStickMouse(const PS4ControllerReport& r) {
        float rx = normalizeAxis(r.rightStickX);
        float ry = normalizeAxis(r.rightStickY);
        // apply a small deadzone
        const float stickDead = 0.12f;
        int moveX = 0, moveY = 0;
        if (std::fabs(rx) > stickDead || std::fabs(ry) > stickDead) {
            auto scale = [](float v)->float {
                float s = std::copysign(v * v * v, v);
                return s;
            };
            float sx = scale(rx);
            float sy = scale(ry);
            const float sensitivity = 14.0f; // pixels per frame at full tilt
            moveX = static_cast<int>(std::round(sx * sensitivity));
            moveY = static_cast<int>(std::round(sy * sensitivity));
            if (moveX == 0 && std::fabs(rx) > stickDead) moveX = (rx > 0) ? 1 : -1;
            if (moveY == 0 && std::fabs(ry) > stickDead) moveY = (ry > 0) ? 1 : -1;
        }

        if (moveX != 0 || moveY != 0) {
            Emu::sendMouseMoveRelative(moveX, moveY);
            lastMouseMoveX = moveX;
            lastMouseMoveY = moveY;
        } else {
            lastMouseMoveX = lastMouseMoveY = 0;
        }
    }

    // Virtual keyboard processing
    void processVirtualKeyboard(const PS4ControllerReport& r) {
        // Detect face button rising edges for virtual keyboard actions
        bool square = (r.buttons1 & 0x10) != 0;
        bool cross  = (r.buttons1 & 0x20) != 0;
        bool circle = (r.buttons1 & 0x40) != 0;
        bool tri    = (r.buttons1 & 0x80) != 0;

        // Move selection with left stick, with a small repeat cooldown
        float lx = normalizeAxis(r.leftStickX);
        float ly = normalizeAxis(r.leftStickY);
        ly = -ly; // stick up -> positive

        const float vkDead = 0.35f;
        auto now = std::chrono::steady_clock::now();
        auto msSince = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastVKMove).count();
        if (msSince >= vkMoveDelayMs) {
            // choose dominant direction
            if (std::fabs(lx) > std::fabs(ly)) {
                if (lx > vkDead) { moveVKSelection(1, 0); lastVKMove = now; }
                else if (lx < -vkDead) { moveVKSelection(-1, 0); lastVKMove = now; }
            } else {
                if (ly > vkDead) { moveVKSelection(0, -1); lastVKMove = now; } // up decreases row index
                else if (ly < -vkDead) { moveVKSelection(0, 1); lastVKMove = now; } // down increases row
            }
        }

        // Face buttons: use rising edge detection using controllerPrev
        // Cross: press selected key
        if (cross && !controllerPrev["CROSS"]) {
            pressSelectedVirtualKey();
        }
        // Square: toggle shift (sticky)
        if (square && !controllerPrev["SQUARE"]) {
            toggleShiftSticky();
        }
        // Circle: Backspace
        if (circle && !controllerPrev["CIRCLE"]) {
            pressVirtualKeyByLabel("BACKSPACE");
        }
        // Triangle: Space
        if (tri && !controllerPrev["TRIANGLE"]) {
            pressVirtualKeyByLabel("SPACE");
        }

        // update controllerPrev
        controllerPrev["CROSS"] = cross;
        controllerPrev["SQUARE"] = square;
        controllerPrev["CIRCLE"] = circle;
        controllerPrev["TRIANGLE"] = tri;
    }

    void moveVKSelection(int dx, int dy) {
        // dx >0 move right; dy >0 move down
        int newRow = selRow + dy;
        if (newRow < 0) newRow = 0;
        if (newRow >= vkRows) newRow = vkRows - 1;
        int maxCols = static_cast<int>(vkLayout[newRow].size());
        int newCol = selCol + dx;
        if (newCol < 0) newCol = 0;
        if (newCol >= maxCols) newCol = maxCols - 1;
        selRow = newRow;
        selCol = newCol;
    }

    void pressSelectedVirtualKey() {
        if (selRow < 0 || selRow >= vkRows) return;
        if (selCol < 0 || selCol >= static_cast<int>(vkLayout[selRow].size())) return;
        const std::string &label = vkLayout[selRow][selCol];
        pressVirtualKeyByLabel(label);
    }

    WORD getVkForLabel(const std::string &label) {
        if (label.empty()) return 0;
        if (label.size() == 1) {
            char c = label[0];
            if (std::isalpha(static_cast<unsigned char>(c))) {
                return static_cast<WORD>(std::toupper(static_cast<unsigned char>(c)));
            }
            if (std::isdigit(static_cast<unsigned char>(c))) {
                return static_cast<WORD>(c); // '0'..'9' map to VK 0x30..0x39
            }
        }
        // specials
        if (label == "SPACE") return VK_SPACE;
        if (label == "ENTER") return VK_RETURN;
        if (label == "BACKSPACE") return VK_BACK;
        if (label == "TAB") return VK_TAB;
        if (label == "CAPS") return VK_CAPITAL;
        if (label == "LSHFT" || label == "RSHIFT") return VK_LSHIFT;
        if (label == "LCTRL" || label == "RCTRL") return VK_LCONTROL;
        if (label == "LALT" || label == "RALT") return VK_MENU;
        // OEM / punctuation
        if (label == ",") return 0xBC; // VK_OEM_COMMA
        if (label == ".") return 0xBE; // VK_OEM_PERIOD
        if (label == "/") return 0xBF; // VK_OEM_2
        if (label == ";") return 0xBA; // VK_OEM_1
        if (label == "'") return 0xDE; // VK_OEM_7
        if (label == "[") return 0xDB; // VK_OEM_4
        if (label == "]") return 0xDD; // VK_OEM_6
        if (label == "\\") return 0xDC; // VK_OEM_5
        if (label == "-") return 0xBD; // VK_OEM_MINUS
        if (label == "=") return 0xBB; // VK_OEM_PLUS
        return 0;
    }

    void pressVirtualKeyByLabel(const std::string &label) {
        WORD vk = getVkForLabel(label);
        if (vk == 0) return;

        // If shiftSticky is enabled and not currently physically held by the system via us,
        // ensure Shift is held during the press.
        if (shiftSticky) {
            // Ensure shift key is held by emulator
            setShiftState(true);
            // send letter key
            Emu::sendKey(vk, true);
            Emu::sendKey(vk, false);
            // do not clear shiftSticky; it remains until toggled off
        } else {
            // normal momentary press
            Emu::sendKey(vk, true);
            Emu::sendKey(vk, false);
        }
    }

    void toggleShiftSticky() {
        shiftSticky = !shiftSticky;
        setShiftState(shiftSticky);
    }

    void setShiftState(bool on) {
        if (on && !shiftHeldByEmulator) {
            Emu::sendKey(VK_LSHIFT, true);
            shiftHeldByEmulator = true;
        } else if (!on && shiftHeldByEmulator) {
            Emu::sendKey(VK_LSHIFT, false);
            shiftHeldByEmulator = false;
        }
    }

    // Track which virtual keys are currently held by us to avoid repeated down events
    void setKeyState(WORD vk, bool wantDown) {
        auto it = keyState.find(vk);
        bool currentlyDown = (it != keyState.end()) ? it->second : false;
        if (wantDown && !currentlyDown) {
            Emu::sendKey(vk, true);
            keyState[vk] = true;
            // start repeat timer for repeatable keys
            if (std::find(repeatKeys.begin(), repeatKeys.end(), vk) != repeatKeys.end()) {
                repeatNextTime[vk] = std::chrono::steady_clock::now() + std::chrono::milliseconds(repeatInitialDelayMs);
            }
        } else if (!wantDown && currentlyDown) {
            Emu::sendKey(vk, false);
            keyState[vk] = false;
            // stop repeating
            repeatNextTime.erase(vk);
        }
    }

    void setMouseButtonState(bool left, bool wantDown) {
        bool &stateRef = left ? mouseLeftDown : mouseRightDown;
        if (wantDown && !stateRef) {
            Emu::sendMouseButton(left, true);
            stateRef = true;
        } else if (!wantDown && stateRef) {
            Emu::sendMouseButton(left, false);
            stateRef = false;
        }
    }

    void releaseAllInputs() {
        // release keys we might be holding
        for (auto &kv : keyState) {
            if (kv.second) {
                Emu::sendKey(kv.first, false);
                kv.second = false;
            }
        }
        keyState.clear();
        // clear any repeat timers/state
        repeatNextTime.clear();

        if (mouseLeftDown) {
            Emu::sendMouseButton(true, false);
            mouseLeftDown = false;
        }
        if (mouseRightDown) {
            Emu::sendMouseButton(false, false);
            mouseRightDown = false;
        }

        for (auto &kv : faceButtonState) {
            if (kv.second) {
                WORD vk = faceButtonMap[kv.first];
                Emu::sendKey(vk, false);
                kv.second = false;
            }
        }

        // release shift if emulator is holding it
        if (shiftHeldByEmulator) {
            Emu::sendKey(VK_LSHIFT, false);
            shiftHeldByEmulator = false;
        }
    }

    // Called regularly from run() to synthesize repeated keypresses for held keys
    void handleKeyRepeats() {
        auto now = std::chrono::steady_clock::now();
        for (WORD vk : repeatKeys) {
            auto itState = keyState.find(vk);
            if (itState == keyState.end() || !itState->second) {
                // not held -> nothing to do
                continue;
            }
            auto itNext = repeatNextTime.find(vk);
            if (itNext == repeatNextTime.end()) {
                // schedule initial repeat if somehow missing
                repeatNextTime[vk] = now + std::chrono::milliseconds(repeatInitialDelayMs);
                continue;
            }
            if (now >= itNext->second) {
                // simulate a repeat by sending a quick up->down (keeps our "held" semantic in keyState)
                Emu::sendKey(vk, false);
                Emu::sendKey(vk, true);
                // schedule next repeat
                itNext->second = now + std::chrono::milliseconds(repeatIntervalMs);
            }
        }
    }

    // ---------- UI / rendering ----------
    void printHeader() {
        console.setCursor(0, 0);
        std::cout << "=== PS4 Controller -> Mouse/Keyboard Mapper ===\n";
        std::cout << "Mappings (Visualizer mode):\n";
        std::cout << "  Left stick -> WASD (analog -> digital)\n";
        std::cout << "  D-Pad -> Arrow keys\n";
        std::cout << "  Right stick -> Mouse movement (relative)\n";
        std::cout << "  R2 -> Left mouse button, L2 -> Right mouse button\n";
        std::cout << "Controls:\n";
        std::cout << "  ESC to exit | TAB to toggle Visualizer/Virtual Keyboard | OPTIONS button toggles too\n";
        std::cout << "  In Virtual Keyboard: Left stick to move, Cross(X) to press, Square toggles Shift, Circle Backspace, Triangle Space\n\n";
        std::cout.flush();
    }

    void updateDisplay() {
        console.clear();
        printHeader();
        std::optional<PS4ControllerReport> snapshot;
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            snapshot = lastReport;
        }

        // Mode indicator
        console.writeAt(0, 7, std::string("Mode: ") + (mode == MODE_VISUALIZER ? "Visualizer" : "Virtual Keyboard"));

        if (!snapshot.has_value()) {
            console.writeAt(0, 9, "Waiting for controller data...");
            return;
        }
        const PS4ControllerReport &r = snapshot.value();

        if (mode == MODE_VISUALIZER) {
            drawStick(0, 10, r.leftStickX, r.leftStickY, "Left");
            drawStick(30, 10, r.rightStickX, r.rightStickY, "Right");
            drawTrigger(60, 10, r.leftTrigger, "L2");
            drawTrigger(60, 11, r.rightTrigger, "R2");
            console.writeAt(60, 13, "Battery: " + padNumber((int)r.battery, 3));
            drawButtons(0, 18, r);
            console.writeAt(0, 26, "Last mouse move: X=" + std::to_string(lastMouseMoveX) + " Y=" + std::to_string(lastMouseMoveY));
            console.writeAt(0, 27, "Mouse L down: " + std::string(mouseLeftDown ? "YES" : "NO") + "  Mouse R down: " + std::string(mouseRightDown ? "YES" : "NO"));
            constexpr size_t HEX_DUMP_BYTES = 24;
            console.writeAt(0, 29, "Raw Data: " + Console::bytesToHex(reinterpret_cast<const uint8_t*>(&r), min(sizeof(r), HEX_DUMP_BYTES)));
        } else {
            // Virtual keyboard UI
            drawVirtualKeyboard(0, 10);
            console.writeAt(0, 18 + vkRows + 1, "Shift (Square): " + std::string(shiftSticky ? "ON" : "OFF"));
            console.writeAt(0, 20 + vkRows + 1, "Press Cross to send selected key. Circle = Backspace, Triangle = Space. TAB/OPTIONS toggles mode.");
            console.writeAt(0, 22 + vkRows + 1, "Last mouse move: X=" + std::to_string(lastMouseMoveX) + " Y=" + std::to_string(lastMouseMoveY));
            constexpr size_t HEX_DUMP_BYTES = 24;
            console.writeAt(0, 24 + vkRows + 1, "Raw Data: " + Console::bytesToHex(reinterpret_cast<const uint8_t*>(&r), min(sizeof(r), HEX_DUMP_BYTES)));
        }
    }

    void drawStick(int x, int y, uint8_t rawX, uint8_t rawY, const std::string& name) {
        constexpr int GRID_W = 11; // -5..+5
        constexpr int GRID_H = 5;
        constexpr int HALF_W = 5;
        constexpr int HALF_H = 2;

        console.writeAt(x, y, name + " Stick:");

        auto norm = [](uint8_t v) -> float {
            return (static_cast<int>(v) - 128) / 127.0f;
        };
        float nx = norm(rawX);
        float ny = norm(rawY);
        int posX = static_cast<int>(std::round(nx * HALF_W));
        int posY = static_cast<int>(std::round(ny * HALF_H));

        for (int row = -HALF_H; row <= HALF_H; ++row) {
            std::string line;
            line.reserve(GRID_W);
            for (int col = -HALF_W; col <= HALF_W; ++col) {
                if (col == posX && row == posY) line += "@";
                else if (col == 0 && row == 0) line += "+";
                else line += ".";
            }
            console.writeAt(x, y + 1 + (row + HALF_H), line);
        }
        console.writeAt(x, y + 1 + GRID_H, "X: " + padNumber((int)rawX, 3) + " Y: " + padNumber((int)rawY, 3));
    }

    void drawTrigger(int x, int y, uint8_t value, const std::string& name) {
        int bars = (value * 10) / 255;
        std::ostringstream ss;
        ss << name << ": [";
        for (int i = 0; i < 10; ++i) ss << (i < bars ? "#" : ".");
        ss << "] " << std::setw(3) << (int)value;
        console.writeAt(x, y, ss.str());
    }

    void drawButtons(int x, int y, const PS4ControllerReport& r) {
        std::ostringstream ss1;
        ss1 << "Buttons: ";
        ss1 << (r.buttons1 & 0x10 ? "[SQR] " : " SQR  ");
        ss1 << (r.buttons1 & 0x20 ? "[CRO] " : " CRO  ");
        ss1 << (r.buttons1 & 0x40 ? "[CIR] " : " CIR  ");
        ss1 << (r.buttons1 & 0x80 ? "[TRI] " : " TRI  ");
        console.writeAt(x, y, ss1.str());

        uint8_t dpad = r.buttons1 & 0x0F;
        std::string dpadStr = dpadToLabel(dpad);
        console.writeAt(x, y + 1, "D-Pad: " + dpadStr);

        std::ostringstream ss2;
        ss2 << (r.buttons2 & 0x01 ? "[L1] " : " L1  ");
        ss2 << (r.buttons2 & 0x02 ? "[R1] " : " R1  ");
        ss2 << (r.buttons2 & 0x40 ? "[L3] " : " L3  ");
        ss2 << (r.buttons2 & 0x80 ? "[R3] " : " R3  ");
        ss2 << " | ";
        ss2 << (r.buttons3 & 0x01 ? "[PS] " : " PS  ");
        ss2 << (r.buttons3 & 0x02 ? "[PAD] " : " PAD  ");
        ss2 << (r.buttons2 & 0x10 ? "[SHARE] " : " SHARE  ");
        ss2 << (r.buttons2 & 0x20 ? "[OPTIONS] " : " OPTIONS  ");

        console.writeAt(x, y + 2, ss2.str());
    }

    void drawVirtualKeyboard(int x, int y) {
        // draw the vkLayout with the selected key highlighted by [label]
        for (int r = 0; r < vkRows; ++r) {
            int colX = x;
            for (size_t c = 0; c < vkLayout[r].size(); ++c) {
                std::string label = vkLayout[r][c];
                std::string disp;
                // pad each key to width 7
                const int minKeyWidth = 7;
                if (r == selRow && static_cast<int>(c) == selCol) {
                    // selected
                    std::ostringstream ss;
                    ss << '[' << label << ']';
                    disp = ss.str();
                } else {
                    disp = " " + label + " ";
                }
                // pad/truncate to minKeyWidth
                if ((int)disp.size() < minKeyWidth) disp += std::string(minKeyWidth - (int)disp.size(), ' ');
                console.writeAt(colX, y + r, disp);
                colX += disp.size() + 1;
            }
        }
    }

    static std::string dpadToLabel(uint8_t d) {
        switch (d) {
            case 0: return "Up";
            case 1: return "Up-Right";
            case 2: return "Right";
            case 3: return "Down-Right";
            case 4: return "Down";
            case 5: return "Down-Left";
            case 6: return "Left";
            case 7: return "Up-Left";
            default: return "Neutral";
        }
    }

    static std::string padNumber(int v, int w) {
        std::ostringstream ss;
        ss << std::setw(w) << v;
        return ss.str();
    }

    void toggleMode() {
        if (mode == MODE_VISUALIZER) setMode(MODE_VKEYBOARD);
        else setMode(MODE_VISUALIZER);
    }

    void setMode(Mode m) {
        if (mode == m) return;
        // switching modes - release inputs to avoid stuck keys/buttons
        releaseAllInputs();
        mode = m;
        // if entering keyboard mode, ensure selection within bounds
        if (mode == MODE_VKEYBOARD) {
            if (selRow < 0) selRow = 0;
            if (selRow >= vkRows) selRow = vkRows - 1;
            if (selCol < 0) selCol = 0;
            if (selCol >= static_cast<int>(vkLayout[selRow].size())) selCol = static_cast<int>(vkLayout[selRow].size()) - 1;
        }
        updateDisplay();
    }

private:
    HWND hwnd = nullptr;
    const std::wstring windowClassName = L"PS4RawInputClassRefactored";

    std::mutex stateMutex;
    std::optional<PS4ControllerReport> lastReport;
    bool controllerConnected = false;

    Console console;

    // Mapping state
    std::map<WORD, bool> keyState; // virtual-key -> currently down?
    bool mouseLeftDown = false;
    bool mouseRightDown = false;
    int lastMouseMoveX = 0;
    int lastMouseMoveY = 0;

    // Face button mapping (visualizer)
    std::map<std::string, WORD> faceButtonMap;
    std::map<std::string, bool> faceButtonState;

    // Controller previous button states (for edge detection in VK mode)
    std::map<std::string, bool> controllerPrev;
    bool prevOptions = false;

    // Mode
    Mode mode = MODE_VISUALIZER;

    // Virtual keyboard layout and selection
    std::vector<std::vector<std::string>> vkLayout;
    int vkRows = 0;
    int selRow = 0, selCol = 0;
    int vkMoveDelayMs = 150;
    std::chrono::steady_clock::time_point lastVKMove;

    // Shift sticky state in virtual keyboard
    bool shiftSticky = false;
    bool shiftHeldByEmulator = false;

    // --- Repeat support for held keys (WASD + arrows) ---
    // Keys we want to auto-repeat while held
    std::vector<WORD> repeatKeys = { VK_KEY_W, VK_KEY_A, VK_KEY_S, VK_KEY_D, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    // Next scheduled repeat time per key
    std::map<WORD, std::chrono::steady_clock::time_point> repeatNextTime;
    // initial delay and repeat interval (ms)
    int repeatInitialDelayMs = 300;
    int repeatIntervalMs = 70;

};

// helper: virtual key constants for arrows (already in WinAPI)
#ifndef VK_KEY_W
#define VK_KEY_W 0x57
#define VK_KEY_A 0x41
#define VK_KEY_S 0x53
#define VK_KEY_D 0x44
#endif

int main() {
    // Note: enabling SetProcessDPIAware may be useful if cursor movement appears scaled,
    // but sending relative moves is DPI-independent. Uncomment if desired:
    // SetProcessDPIAware();

    try {
        PS4VisualizerMapper viz;
        viz.run();
    } catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << std::endl;
        return 1;
    }
    return 0;
}