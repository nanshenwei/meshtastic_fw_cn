#include "configuration.h"
#if HAS_SCREEN

#if defined(M5STACK_CARDPUTER_ADV)
// Access private members of VirtualKeyboard to support physical keyboard input
#define private public
#include "graphics/VirtualKeyboard.h"
#undef private
#endif

#include "graphics/SharedUIDisplay.h"
#include "graphics/draw/NotificationRenderer.h"
#include "input/RotaryEncoderInterruptImpl1.h"
#include "input/UpDownInterruptImpl1.h"
#include "modules/OnScreenKeyboardModule.h"
#include <Arduino.h>
#include <algorithm>

namespace graphics
{

OnScreenKeyboardModule &OnScreenKeyboardModule::instance()
{
    static OnScreenKeyboardModule inst;
    return inst;
}

OnScreenKeyboardModule::~OnScreenKeyboardModule()
{
    if (keyboard) {
        delete keyboard;
        keyboard = nullptr;
    }
}

void OnScreenKeyboardModule::start(const char *header, const char *initialText, uint32_t durationMs,
                                   std::function<void(const std::string &)> cb)
{
    if (keyboard) {
        delete keyboard;
        keyboard = nullptr;
    }
    keyboard = new VirtualKeyboard();
    callback = cb;
    if (header)
        keyboard->setHeader(header);
    if (initialText)
        keyboard->setInputText(initialText);

    // Route VK submission/cancel events back into the module
    keyboard->setCallback([this](const std::string &text) {
        if (text.empty()) {
            this->onCancel();
        } else {
            this->onSubmit(text);
        }
    });

    // Maintain legacy compatibility hooks
    NotificationRenderer::virtualKeyboard = keyboard;
    NotificationRenderer::textInputCallback = callback;
}

void OnScreenKeyboardModule::stop(bool callEmptyCallback)
{
    auto cb = callback;
    callback = nullptr;
    if (keyboard) {
        delete keyboard;
        keyboard = nullptr;
    }
    // Keep NotificationRenderer legacy pointers in sync
    NotificationRenderer::virtualKeyboard = nullptr;
    NotificationRenderer::textInputCallback = nullptr;
    clearPopup();
    if (callEmptyCallback && cb)
        cb("");
}

void OnScreenKeyboardModule::handleInput(const InputEvent &event)
{
    if (!keyboard)
        return;

    if (processVirtualKeyboardInput(event, keyboard))
        return;

    if (event.inputEvent == INPUT_BROKER_CANCEL)
        onCancel();
}

bool OnScreenKeyboardModule::processVirtualKeyboardInput(const InputEvent &event, VirtualKeyboard *targetKeyboard)
{
    LOG_INPUT("OSK processInput event=%d char=%d", event.inputEvent, event.kbchar);
    if (!targetKeyboard)
        return false;

    switch (event.inputEvent) {
    case INPUT_BROKER_UP:
    case INPUT_BROKER_UP_LONG:
        targetKeyboard->moveCursorUp();
        return true;
    case INPUT_BROKER_DOWN:
    case INPUT_BROKER_DOWN_LONG:
        targetKeyboard->moveCursorDown();
        return true;
    case INPUT_BROKER_LEFT:
    case INPUT_BROKER_ALT_PRESS:
        targetKeyboard->moveCursorLeft();
        return true;
    case INPUT_BROKER_RIGHT:
    //case INPUT_BROKER_USER_PRESS:
        targetKeyboard->moveCursorRight();
        return true;
    case INPUT_BROKER_SELECT:
#if defined(M5STACK_CARDPUTER_ADV)
        // For Cardputer, SELECT (Enter) should submit text, not press virtual key '1'
        targetKeyboard->submitText();
#else
        targetKeyboard->handlePress();
#endif
        return true;
    case INPUT_BROKER_SELECT_LONG:
        targetKeyboard->handleLongPress();
        return true;
    case INPUT_BROKER_USER_PRESS:
        targetKeyboard->toggleIME();
        return true;
#if defined(M5STACK_CARDPUTER_ADV)
    case INPUT_BROKER_CANCEL:
        // Handle cancel via module to ensure proper cleanup (avoid double-free in NotificationRenderer)
        OnScreenKeyboardModule::instance().onCancel();
        return true;
    case INPUT_BROKER_BACK:
        targetKeyboard->deleteCharacter();
        return true;
    case INPUT_BROKER_ANYKEY:
        if (event.kbchar == 0x08) {
            targetKeyboard->deleteCharacter();
            return true;
        }
        if (event.kbchar == 0x0D || event.kbchar == 0x0A) {
            targetKeyboard->submitText();
            return true;
        }
        if (event.kbchar >= 32 && event.kbchar <= 126) {
            targetKeyboard->insertCharacter((char)event.kbchar);
            return true;
        }
        return false;
#endif
    default:
        return false;
    }
}

bool OnScreenKeyboardModule::draw(OLEDDisplay *display)
{
    if (!keyboard)
        return false;

    // Timeout
    if (keyboard->isTimedOut()) {
        onCancel();
        return false;
    }

    // Clear full screen behind keyboard
    display->setColor(BLACK);
    display->fillRect(0, 0, display->getWidth(), display->getHeight());
    display->setColor(WHITE);
    keyboard->draw(display, 0, 0);

    // Draw popup overlay if needed
    drawPopup(display);
    return true;
}

void OnScreenKeyboardModule::onSubmit(const std::string &text)
{
    auto cb = callback;
    stop(false);
    if (cb)
        cb(text);
}

void OnScreenKeyboardModule::onCancel()
{
    stop(true);
}

void OnScreenKeyboardModule::showPopup(const char *title, const char *content, uint32_t durationMs)
{
    if (!title || !content)
        return;
    strncpy(popupTitle, title, sizeof(popupTitle) - 1);
    popupTitle[sizeof(popupTitle) - 1] = '\0';
    strncpy(popupMessage, content, sizeof(popupMessage) - 1);
    popupMessage[sizeof(popupMessage) - 1] = '\0';
    popupUntil = millis() + durationMs;
    popupVisible = true;
}

void OnScreenKeyboardModule::clearPopup()
{
    popupTitle[0] = '\0';
    popupMessage[0] = '\0';
    popupUntil = 0;
    popupVisible = false;
}

void OnScreenKeyboardModule::drawPopupOverlay(OLEDDisplay *display)
{
    // Only render the popup overlay (without drawing the keyboard)
    drawPopup(display);
}

void OnScreenKeyboardModule::drawPopup(OLEDDisplay *display)
{
    if (!popupVisible)
        return;
    if (millis() > popupUntil || popupMessage[0] == '\0') {
        popupVisible = false;
        return;
    }

    // Build lines and leverage NotificationRenderer inverted box drawing for consistent style
    constexpr uint16_t maxContentLines = 3;
    const bool hasTitle = popupTitle[0] != '\0';

    display->setFont(FONT_SMALL);
    display->setTextAlignment(TEXT_ALIGN_LEFT);
    const uint16_t maxWrapWidth = display->width() - 40;

    auto wrapText = [&](const char *text, uint16_t availableWidth) -> std::vector<std::string> {
        std::vector<std::string> wrapped;
        std::string current;
        std::string word;
        const char *p = text;
        while (*p && wrapped.size() < maxContentLines) {
            while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
                if (*p == '\n') {
                    if (!current.empty()) {
                        wrapped.push_back(current);
                        current.clear();
                        if (wrapped.size() >= maxContentLines)
                            break;
                    }
                }
                ++p;
            }
            if (!*p || wrapped.size() >= maxContentLines)
                break;
            word.clear();
            while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
                word += *p++;
            if (word.empty())
                continue;
            std::string test = current.empty() ? word : (current + " " + word);
            uint16_t w = display->getStringWidth(test.c_str(), test.length(), true);
            if (w <= availableWidth)
                current = test;
            else {
                if (!current.empty()) {
                    wrapped.push_back(current);
                    current = word;
                    if (wrapped.size() >= maxContentLines)
                        break;
                } else {
                    current = word;
                    while (current.size() > 1 &&
                           display->getStringWidth(current.c_str(), current.length(), true) > availableWidth)
                        current.pop_back();
                }
            }
        }
        if (!current.empty() && wrapped.size() < maxContentLines)
            wrapped.push_back(current);
        return wrapped;
    };

    std::vector<std::string> allLines;
    if (hasTitle)
        allLines.emplace_back(popupTitle);

    char buf[sizeof(popupMessage)];
    strncpy(buf, popupMessage, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *paragraph = strtok(buf, "\n");
    while (paragraph && allLines.size() < maxContentLines + (hasTitle ? 1 : 0)) {
        auto wrapped = wrapText(paragraph, maxWrapWidth);
        for (const auto &ln : wrapped) {
            if (allLines.size() >= maxContentLines + (hasTitle ? 1 : 0))
                break;
            allLines.push_back(ln);
        }
        paragraph = strtok(nullptr, "\n");
    }

    std::vector<const char *> ptrs;
    for (const auto &ln : allLines)
        ptrs.push_back(ln.c_str());
    ptrs.push_back(nullptr);

    // Use the standard notification box drawing from NotificationRenderer
    NotificationRenderer::drawNotificationBox(display, nullptr, ptrs.data(), allLines.size(), 0, 0);
}

} // namespace graphics

#endif // HAS_SCREEN
