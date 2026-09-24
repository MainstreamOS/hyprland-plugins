#include "barDeco.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/LayerState.hpp>
#include <hyprland/src/desktop/state/ViewHitTester.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/managers/SessionLockManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/parserUtils/ParserUtils.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include "globals.hpp"
#include "BarPassElement.hpp"

#include <climits>
#include <cmath>
#include <dlfcn.h>
#include <linux/input-event-codes.h>

using namespace Render::GL;

class IOverview;

// Scroll travel is counted in the 120ths of a notch a wheel reports. They are
// whole numbers, so the pieces a high-resolution wheel splits a notch into add
// up to exactly one notch, where adding them as fractions can fall just short.
// A notch is 15 in the smooth units a touchpad reports in too, so a touchpad
// takes a step after about a notch of travel.
constexpr double WHEEL_NOTCH  = 120.0;
constexpr double SMOOTH_NOTCH = 15.0;

// Longer than a spinning wheel rests between notches, and short enough that a
// deliberate second notch still takes a second step.
constexpr uint32_t SCROLL_PAUSE_MS = 280;

static CHyprColor configColor(Config::INTEGER color) {
    return CHyprColor{static_cast<uint64_t>(color)};
}

static bool anyHoverColor() {
    return std::ranges::any_of(g_pGlobalState->buttonHoverColors, [](const auto& c) { return c.has_value(); });
}

// Caps Lock and Num Lock are locks left on, not keys being held.
static bool modifiersHeld() {
    return (g_pInputManager->getModsFromAllKBs() & ~(HL_MODIFIER_CAPS | HL_MODIFIER_MOD2)) != 0;
}

// Scroll Overview draws over the windows without moving them, so a bar still
// hit-tests where its window really is, underneath the overview. Its own
// handlers skip input a bar has already canceled, and the bars hear input
// first, so a gesture has to stand aside wherever an overview is showing.
static bool overviewAtPointer() {
    using PScrollOverviewAt = SP<IOverview> (*)(const Vector2D&);

    if (!g_pPluginSystem)
        return false;

    for (const auto* plugin : g_pPluginSystem->getAllPlugins()) {
        if (!plugin || !plugin->m_handle)
            continue;

        const auto SCROLLOVERVIEWAT = reinterpret_cast<PScrollOverviewAt>(dlsym(plugin->m_handle, "_Z16scrollOverviewAtRKN9Hyprutils4Math8Vector2DE"));

        if (SCROLLOVERVIEWAT)
            return !!SCROLLOVERVIEWAT(g_pInputManager->getMouseCoordsInternal());
    }

    return false;
}

// The window in sight under the pointer. Hyprland's own hit test also counts
// the resize strip a floating window keeps past its edges, which cannot be
// seen, so a window found only by its strip gives way to the one beneath it.
static PHLWINDOW windowInSight(const Vector2D& pos) {
    constexpr auto          FLAGS = Desktop::View::RESERVED_EXTENTS | Desktop::View::ALLOW_FLOATING;
    Desktop::CViewHitTester hitTester{*Desktop::viewState()};

    const auto              WINDOW = hitTester.windowAt(pos, FLAGS);

    if (WINDOW && !WINDOW->getWindowBoxUnified(Desktop::View::RESERVED_EXTENTS).containsPoint(pos) && !WINDOW->hasPopupAt(pos))
        return hitTester.windowAt(pos, FLAGS, WINDOW);

    return WINDOW;
}

CHyprBar::CHyprBar(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow) {
    m_pWindow = pWindow;

    const auto PMONITOR         = pWindow->m_monitor.lock();
    PMONITOR->m_scheduledRecalc = true;

    // button events
    m_pMouseButtonCallback = Event::bus()->m_events.input.mouse.button.listen([&](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(info, e); });
    m_pTouchDownCallback   = Event::bus()->m_events.input.touch.down.listen([&](ITouch::SDownEvent e, Event::SCallbackInfo& info) { onTouchDown(info, e); });
    m_pTouchUpCallback     = Event::bus()->m_events.input.touch.up.listen([&](ITouch::SUpEvent e, Event::SCallbackInfo& info) { onTouchUp(info, e); });

    // move events
    m_pTouchMoveCallback = Event::bus()->m_events.input.touch.motion.listen([&](ITouch::SMotionEvent e, Event::SCallbackInfo& info) { onTouchMove(info, e); });
    m_pMouseMoveCallback = Event::bus()->m_events.input.mouse.move.listen([&](Vector2D c, Event::SCallbackInfo& info) { onMouseMove(c); });

    Animation::mgr()->createAnimation(configColor(g_pGlobalState->config.barColor->value()), m_cRealBarColor, Config::animationTree()->getAnimationPropertyConfig("border"),
                                      pWindow, AVARDAMAGE_NONE);
    m_cRealBarColor->setUpdateCallback([&](auto) { damageEntire(); });

    Animation::mgr()->createAnimation(0.F, m_fButtonsAlpha, Config::animationTree()->getAnimationPropertyConfig("fadeIn"), pWindow, AVARDAMAGE_NONE);
    m_fButtonsAlpha->setUpdateCallback([&](auto) { damageEntire(); });

    Animation::mgr()->createAnimation(0.F, m_fButtonsPop, Config::animationTree()->getAnimationPropertyConfig("windowsIn"), pWindow, AVARDAMAGE_NONE);
    m_fButtonsPop->setUpdateCallback([&](auto) { damageEntire(); });
}

CHyprBar::~CHyprBar() {
    std::erase(g_pGlobalState->bars, m_self);
}

SDecorationPositioningInfo CHyprBar::getPositioningInfo() {
    const auto                 HEIGHT     = g_pGlobalState->config.barHeight->value();
    const auto                 ENABLED    = g_pGlobalState->config.enabled->value();
    const auto                 PRECEDENCE = g_pGlobalState->config.barPrecedenceOverBorder->value();

    SDecorationPositioningInfo info;
    info.policy         = m_hidden ? DECORATION_POSITION_ABSOLUTE : DECORATION_POSITION_STICKY;
    info.edges          = DECORATION_EDGE_TOP;
    info.priority       = PRECEDENCE ? 10005 : 5000;
    info.reserved       = true;
    info.desiredExtents = {{0, m_hidden || !ENABLED ? 0 : HEIGHT}, {0, 0}};
    return info;
}

void CHyprBar::onPositioningReply(const SDecorationPositioningReply& reply) {
    if (reply.assignedGeometry.size() != m_bAssignedBox.size())
        m_bWindowSizeChanged = true;

    m_bAssignedBox = reply.assignedGeometry;
}

std::string CHyprBar::getDisplayName() {
    return "Hyprbar";
}

bool CHyprBar::inputIsValid() {
    if (m_hidden)
        return false;

    // The lock screen is not a layer surface, so the layer checks below never
    // see it, and input on it must not reach the windows it hides.
    if (g_pSessionLockManager->isSessionLocked())
        return false;

    if (g_pSeatManager->m_seatGrab && !g_pSeatManager->m_seatGrab->accepts(m_pWindow->wlSurface()->resource()))
        return false;

    const auto MOUSE    = g_pInputManager->getMouseCoordsInternal();
    auto       PMONITOR = Desktop::focusState()->monitor();

    if (!PMONITOR)
        return false;

    Desktop::CViewHitTester hitTester{*Desktop::viewState()};

    const auto              WINDOWATCURSOR = hitTester.windowAt(MOUSE, Desktop::View::RESERVED_EXTENTS | Desktop::View::INPUT_EXTENTS | Desktop::View::ALLOW_FLOATING);

    auto                    focusState = Desktop::focusState();
    auto                    window     = focusState->window();

    if (WINDOWATCURSOR != m_pWindow && m_pWindow != window)
        return false;

    PHLLS    foundSurface = nullptr;
    Vector2D surfaceCoords;

    // Check Top Layer
    hitTester.layerSurfaceAt(MOUSE, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &foundSurface);
    if (foundSurface)
        return false;

    // Check Overlay Layer
    hitTester.layerSurfaceAt(MOUSE, &PMONITOR->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &foundSurface);
    if (foundSurface)
        return false;

    return true;
}

// Whether the pointer is on this bar where it is drawn and in sight, judged
// the way a click on the bar is, with no popup of the window's own over that
// spot. A click lets the focused window's bar answer from under another
// window, but a gesture there belongs to the window on top, which would get
// the scroll or middle click if the bar were not there.
bool CHyprBar::pointerOnBar() {
    if (m_hidden || !validMapped(m_pWindow) || !g_pGlobalState->config.enabled->value())
        return false;

    const auto MOUSE  = g_pInputManager->getMouseCoordsInternal();
    const auto BOX    = assignedBoxGlobal();
    const auto COORDS = MOUSE - BOX.pos();

    if (!VECINRECT(COORDS, 0, 0, BOX.w, g_pGlobalState->config.barHeight->value() - 1))
        return false;

    const auto PWINDOW = m_pWindow.lock();

    // A fullscreen window's bar is not drawn, yet it keeps its place above the
    // window, which can be out on the monitor next door.
    if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault() || Fullscreen::controller()->getFullscreenModes(PWINDOW).internal == Fullscreen::FSMODE_FULLSCREEN)
        return false;

    return inputIsValid() && windowInSight(MOUSE) == PWINDOW && !PWINDOW->hasPopupAt(MOUSE);
}

// Focus and raise the window the way a click on its bar does, so a command
// that acts on the active window acts on this one. Hyprland can refuse the
// focus or hand it to another window, and then no command may run, since it
// would act on whatever window has the focus instead.
bool CHyprBar::focusWindow() {
    const auto PWINDOW = m_pWindow.lock();

    if (Desktop::focusState()->window() != PWINDOW)
        Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

    if (Desktop::focusState()->window() != PWINDOW)
        return false;

    if (PWINDOW->m_isFloating)
        Desktop::windowState()->raise(PWINDOW);

    return true;
}

// The bar the pointer is on. Only one window is in sight under the pointer, so
// at most one bar can be.
WP<CHyprBar> CHyprBar::barAtPointer() {
    for (const auto& b : g_pGlobalState->bars) {
        if (b && b->pointerOnBar())
            return b;
    }

    return {};
}

void CHyprBar::onMouseAxis(IPointer::SAxisEvent e, Event::SCallbackInfo& info) {
    if (e.axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
        return;

    const auto ON_SCROLL_UP   = g_pGlobalState->onScrollUp->value();
    const auto ON_SCROLL_DOWN = g_pGlobalState->onScrollDown->value();

    if (ON_SCROLL_UP.empty() && ON_SCROLL_DOWN.empty())
        return;

    auto& gesture = g_pGlobalState->scrollGesture;

    // A touchpad reports its fingers lifting as a scroll of nothing, which
    // ends the scroll as surely as a pause does.
    const bool STOPPED = e.source != WL_POINTER_AXIS_SOURCE_WHEEL && e.delta == 0.0;
    const bool PAUSED  = e.timeMs - gesture.lastAxisTimeMs > SCROLL_PAUSE_MS;

    // Whether this event belongs to a scroll that has already moved a window.
    // The lift that ends a scroll is still part of it, while the first event
    // after a pause starts a new one.
    const bool SPENT = gesture.stepped && (STOPPED || !PAUSED);

    if (STOPPED || PAUSED) {
        gesture.armed       = true;
        gesture.stepped     = false;
        gesture.accumulated = 0.0;
    }

    gesture.lastAxisTimeMs = e.timeMs;

    // A scroll that goes elsewhere first, Super and the wheel switching
    // workspaces among them, spends the step too: whatever bar the rest of it
    // meets was not what the scroll was aimed at.
    const bool FREE     = !info.cancelled && !modifiersHeld();
    const auto bar      = FREE ? barAtPointer() : WP<CHyprBar>{};
    const bool OVERVIEW = FREE && overviewAtPointer();

    if (!bar || OVERVIEW) {
        if (!STOPPED)
            gesture.armed = false;

        // A step often takes the bar out from under the pointer and leaves a
        // page or another window there, which must not get the rest of the
        // scroll that moved the window. Super and the wheel still switch
        // workspaces, and Scroll Overview still hears it.
        if (SPENT && FREE && !OVERVIEW)
            info.cancelled = true;

        return;
    }

    info.cancelled = true;

    if (!gesture.armed || STOPPED)
        return;

    const double TRAVEL = e.source == WL_POINTER_AXIS_SOURCE_WHEEL ? e.deltaDiscrete : e.delta * (WHEEL_NOTCH / SMOOTH_NOTCH);

    if (TRAVEL * gesture.accumulated < 0.0)
        gesture.accumulated = 0.0;

    gesture.accumulated += TRAVEL;

    if (std::abs(gesture.accumulated) < WHEEL_NOTCH)
        return;

    const auto CMD = gesture.accumulated < 0.0 ? ON_SCROLL_UP : ON_SCROLL_DOWN;

    gesture.accumulated = 0.0;

    if (CMD.empty())
        return;

    gesture.armed   = false;
    gesture.stepped = true;

    if (bar->focusWindow())
        Config::Supplementary::executor()->spawn(CMD);
}

// A middle click runs on_middle_click when the button comes back up over the
// bar it went down on, so sliding off the bar first calls it off. A press on
// one of the bar's buttons with no overview showing there is handed back to
// the usual handling by returning false; any other middle press is kept from
// starting a drag or counting toward a double click.
bool CHyprBar::onMiddleButton(Event::SCallbackInfo& info, IPointer::SButtonEvent e) {
    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        if (!m_bMiddleDown)
            return false;

        m_bMiddleDown  = false;
        info.cancelled = true;

        const auto ON_MIDDLE_CLICK = g_pGlobalState->onMiddleClick->value();

        if (!ON_MIDDLE_CLICK.empty() && pointerOnBar() && focusWindow())
            Config::Supplementary::executor()->spawn(ON_MIDDLE_CLICK);

        return true;
    }

    if (g_pGlobalState->onMiddleClick->value().empty())
        return false;

    if (!pointerOnBar())
        return true;

    // A press under an overview belongs to the overview, even over a button:
    // the overview hears it only if no bar has taken it first.
    if (overviewAtPointer())
        return true;

    if (hoveredButtons() != 0)
        return false;

    // A press with modifiers held is left to Hyprland's own mouse binds.
    if (modifiersHeld())
        return true;

    info.cancelled = true;
    m_bMiddleDown  = true;
    focusWindow();

    return true;
}

void CHyprBar::onMouseButton(Event::SCallbackInfo& info, IPointer::SButtonEvent e) {
    if (e.button == BTN_MIDDLE && onMiddleButton(info, e))
        return;

    if (!inputIsValid())
        return;

    if (e.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        handleUpEvent(info);
        return;
    }

    handleDownEvent(info, std::nullopt);
}

void CHyprBar::onTouchDown(Event::SCallbackInfo& info, ITouch::SDownEvent e) {
    // Don't do anything if you're already grabbed a window with another finger
    if (!inputIsValid() || e.touchID != 0)
        return;

    handleDownEvent(info, e);
}

void CHyprBar::onTouchUp(Event::SCallbackInfo& info, ITouch::SUpEvent e) {
    if (!m_bDragPending || !m_bTouchEv || e.touchID != m_touchId)
        return;

    handleUpEvent(info);
}

void CHyprBar::onMouseMove(Vector2D coords) {
    // ensure proper redraws of button icons and hover colors on hover when using hardware cursors
    if (g_pGlobalState->config.iconOnHover->value() || anyHoverColor())
        damageOnButtonHover();

    // buttons_on_hover: the buttons only exist while the cursor is over the bar,
    // so the whole bar has to be redrawn as the cursor crosses its edge.
    if (g_pGlobalState->config.buttonsOnHover->value()) {
        const auto BOX     = assignedBoxGlobal();
        const auto COORDS  = cursorRelativeToBar();
        const bool HOVERED = VECINRECT(COORDS, 0, 0, BOX.w, BOX.h);
        if (HOVERED != m_bBarHovered) {
            m_bBarHovered = HOVERED;
            // They fade the way windows do, in and out at the user's own pace.
            m_fButtonsAlpha->setConfig(Config::animationTree()->getAnimationPropertyConfig(HOVERED ? "fadeIn" : "fadeOut"));
            *m_fButtonsAlpha = HOVERED ? 1.F : 0.F;
            // With buttons_pop_in they also move the way windows open and close,
            // on that curve and in that style.
            if (g_pGlobalState->config.buttonsPopIn->value()) {
                m_fButtonsPop->setConfig(Config::animationTree()->getAnimationPropertyConfig(HOVERED ? "windowsIn" : "windowsOut"));
                setButtonsPopStyle(m_fButtonsPop->getStyle());
                *m_fButtonsPop = HOVERED ? 1.F : 0.F;
            } else
                m_fButtonsPop->setValueAndWarp(HOVERED ? 1.F : 0.F);
            damageEntire();
        }
    }

    if (!m_bDragPending || m_bTouchEv || !validMapped(m_pWindow) || m_touchId != 0)
        return;

    m_bDragPending = false;
    handleMovement();
}

void CHyprBar::onTouchMove(Event::SCallbackInfo& info, ITouch::SMotionEvent e) {
    if (!m_bDragPending || !m_bTouchEv || !validMapped(m_pWindow) || e.touchID != m_touchId)
        return;

    auto PMONITOR     = m_pWindow->m_monitor.lock();
    PMONITOR          = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
    const auto COORDS = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y);

    if (!m_bDraggingThis) {
        // Initial setup for dragging a window.
        g_pKeybindManager->m_dispatchers["setfloating"]("activewindow");
        g_pKeybindManager->m_dispatchers["resizewindowpixel"]("exact 50% 50%,activewindow");
        // pin it so you can change workspaces while dragging a window
        g_pKeybindManager->m_dispatchers["pin"]("activewindow");
    }
    g_pKeybindManager->m_dispatchers["movewindowpixel"](std::format("exact {} {},activewindow", (int)(COORDS.x - (assignedBoxGlobal().w / 2)), (int)COORDS.y));
    m_bDraggingThis = true;
}

void CHyprBar::handleDownEvent(Event::SCallbackInfo& info, std::optional<ITouch::SDownEvent> touchEvent) {
    m_bTouchEv = touchEvent.has_value();
    if (m_bTouchEv)
        m_touchId = touchEvent.value().touchID;

    const auto PWINDOW = m_pWindow.lock();

    auto       COORDS = cursorRelativeToBar();
    if (m_bTouchEv) {
        ITouch::SDownEvent e        = touchEvent.value();
        PHLMONITOR         PMONITOR = nullptr;
        for (auto& m : State::monitorState()->monitors()) {
            if (m->m_name == (!e.device->m_boundOutput.empty() ? e.device->m_boundOutput : "")) {
                PMONITOR = m;
                break;
            }
        }
        PMONITOR = PMONITOR ? PMONITOR : Desktop::focusState()->monitor();
        COORDS   = Vector2D(PMONITOR->m_position.x + e.pos.x * PMONITOR->m_size.x, PMONITOR->m_position.y + e.pos.y * PMONITOR->m_size.y) - assignedBoxGlobal().pos();
    }

    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const auto ON_DOUBLE_CLICK  = g_pGlobalState->config.onDoubleClick->value();

    const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";

    if (!VECINRECT(COORDS, 0, 0, assignedBoxGlobal().w, HEIGHT - 1)) {

        if (m_bDraggingThis) {
            if (m_bTouchEv)
                g_pKeybindManager->m_dispatchers["settiled"]("activewindow");
            g_pKeybindManager->m_dispatchers["mouse"]("0movewindow");
            Log::logger->log(Log::DEBUG, "[hyprbars] Dragging ended on {:x}", (uintptr_t)PWINDOW.get());
        }

        m_bDraggingThis = false;
        m_bDragPending  = false;
        m_bTouchEv      = false;
        return;
    }

    // don't swallow clicks meant for xdg popups (e.g. context menus) overlapping the bar
    if (PWINDOW->hasPopupAt(COORDS + assignedBoxGlobal().pos()))
        return;

    if (Desktop::focusState()->window() != PWINDOW)
        Desktop::focusState()->fullWindowFocus(PWINDOW, Desktop::FOCUS_REASON_CLICK);

    if (PWINDOW->m_isFloating)
        Desktop::windowState()->raise(PWINDOW);

    info.cancelled   = true;
    m_bCancelledDown = true;

    if (doButtonPress(BARPADDING, BARBUTTONPADDING, HEIGHT, COORDS, BUTTONSRIGHT))
        return;

    if (!ON_DOUBLE_CLICK.empty() &&
        std::chrono::duration_cast<std::chrono::milliseconds>(Time::steadyNow() - m_lastMouseDown).count() < 400 /* Arbitrary delay I found suitable */) {
        Config::Supplementary::executor()->spawn(ON_DOUBLE_CLICK);
        m_bDragPending = false;
    } else {
        m_lastMouseDown = Time::steadyNow();
        m_bDragPending  = true;
    }
}

void CHyprBar::handleUpEvent(Event::SCallbackInfo& info) {
    if (m_pWindow.lock() != Desktop::focusState()->window())
        return;

    if (m_bCancelledDown)
        info.cancelled = true;

    m_bCancelledDown = false;

    if (m_bDraggingThis) {
        g_pKeybindManager->changeMouseBindMode(MBIND_INVALID);
        m_bDraggingThis = false;
        if (m_bTouchEv)
            (void)Config::Actions::floatWindow(Config::Actions::eTogglableAction::TOGGLE_ACTION_DISABLE);

        Log::logger->log(Log::DEBUG, "[hyprbars] Dragging ended on {:x}", (uintptr_t)m_pWindow.lock().get());
    }

    m_bDragPending = false;
    m_bTouchEv     = false;
    m_touchId      = 0;
}

void CHyprBar::handleMovement() {
    g_pKeybindManager->changeMouseBindMode(MBIND_MOVE);
    m_bDraggingThis = true;
    Log::logger->log(Log::DEBUG, "[hyprbars] Dragging initiated on {:x}", (uintptr_t)m_pWindow.lock().get());
    return;
}

bool CHyprBar::doButtonPress(Config::INTEGER barPadding, Config::INTEGER barButtonPadding, Config::INTEGER barHeight, Vector2D COORDS, const bool BUTTONSRIGHT) {
    //check if on a button
    float offset = barPadding;

    for (auto& b : g_pGlobalState->buttons) {
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, barHeight};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - barButtonPadding - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

        if (VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + b.size + barButtonPadding, currentPos.y + b.size)) {
            // hit on close
            g_pKeybindManager->m_dispatchers["exec"](b.cmd);
            return true;
        }

        offset += barButtonPadding + b.size;
    }
    return false;
}

void CHyprBar::renderBarTitle(const Vector2D& bufferSize, const float scale) {
    const auto COLORVAL         = g_pGlobalState->config.textColor->value();
    const auto SIZE             = g_pGlobalState->config.barTextSize->value();
    const auto WEIGHT           = g_pGlobalState->config.barTextWeight->value();
    const auto FONT             = g_pGlobalState->config.barTextFont->value();
    const auto ALIGN            = g_pGlobalState->config.barTextAlign->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();

    float      buttonSizes = BARBUTTONPADDING;
    for (auto& b : g_pGlobalState->buttons) {
        buttonSizes += b.size + BARBUTTONPADDING;
    }

    const int  scaledSize        = std::round(SIZE * scale);
    const auto scaledButtonsSize = buttonSizes * scale;
    const auto scaledBarPadding  = BARPADDING * scale;
    const int  paddingTotal      = scaledBarPadding * 2 + scaledButtonsSize + (ALIGN != "left" ? scaledButtonsSize : 0);
    const int  maxWidth          = std::clamp(static_cast<int>(bufferSize.x - paddingTotal), 0, INT_MAX);

    if (m_szLastTitle.empty() || maxWidth < 1) {
        m_pTextTex = nullptr;
        return;
    }

    const CHyprColor COLOR = m_bForcedTitleColor.value_or(configColor(COLORVAL));
    m_pTextTex             = g_pHyprRenderer->renderText(m_szLastTitle, COLOR, scaledSize, false, FONT, maxWidth, WEIGHT.m_value);
}

size_t CHyprBar::getVisibleButtonCount(Config::INTEGER barButtonPadding, Config::INTEGER barPadding, const Vector2D& bufferSize, const float scale) {
    float  availableSpace = bufferSize.x - barPadding * scale * 2;
    size_t count          = 0;

    for (const auto& button : g_pGlobalState->buttons) {
        const float buttonSpace = (button.size + barButtonPadding) * scale;
        if (availableSpace >= buttonSpace) {
            count++;
            availableSpace -= buttonSpace;
        } else
            break;
    }

    return count;
}

void CHyprBar::renderBarButtons(CBox* barBox, const float scale, const float a) {
    const float BUTTONSALPHA = g_pGlobalState->config.buttonsOnHover->value() ? m_fButtonsAlpha->value() : 1.F;
    if (BUTTONSALPHA <= 0.F)
        return;

    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const auto INACTIVECOLOR    = g_pGlobalState->config.inactiveButtonColor->value();

    const bool BUTTONSRIGHT    = ALIGNBUTTONS != "left";
    const auto visibleCount    = getVisibleButtonCount(BARBUTTONPADDING, BARPADDING, Vector2D{barBox->w, barBox->h}, scale);
    const bool INVALIDATEICONS = m_bButtonsDirty || m_bWindowSizeChanged;
    const auto HOVERED         = anyHoverColor() ? hoveredButtons() : 0U;
    const bool POPPING         = buttonsPopping();
    const auto CLIP            = POPPING && m_eButtonsPopStyle == BUTTONS_POP_SLIDE ? std::optional<CRegion>{buttonsPopClip(*barBox)} : std::nullopt;

    int        offset = BARPADDING * scale;
    for (size_t i = 0; i < visibleCount; ++i) {
        auto&      button           = g_pGlobalState->buttons[i];
        const auto scaledButtonSize = button.size * scale;
        const auto scaledButtonsPad = BARBUTTONPADDING * scale;

        auto       color = button.bgcol;

        if (INACTIVECOLOR > 0) {
            color = m_bWindowHasFocus ? color : configColor(INACTIVECOLOR);
            if (INVALIDATEICONS && button.userfg && button.iconTex)
                button.iconTex = nullptr;
        }

        // The hover color answers the cursor, so it outranks the inactive color.
        if (i < 32 && (HOVERED & (1U << i)) && i < g_pGlobalState->buttonHoverColors.size() && g_pGlobalState->buttonHoverColors[i])
            color = *g_pGlobalState->buttonHoverColors[i];

        color.a *= a * BUTTONSALPHA;

        CBox buttonBox = {barBox->x + (BUTTONSRIGHT ? barBox->w - offset - scaledButtonSize : offset), barBox->y + (barBox->h - scaledButtonSize) / 2.0, scaledButtonSize,
                          scaledButtonSize};
        if (POPPING)
            buttonBox = buttonsPopBox(buttonBox, scaledButtonSize, BUTTONSRIGHT);
        buttonBox.round();

        if (buttonBox.w >= 1 && buttonBox.h >= 1)
            g_pHyprOpenGL->renderRect(
                buttonBox, color,
                {.damage = CLIP ? &*CLIP : nullptr, .round = static_cast<int>(std::round((POPPING ? buttonBox.w : scaledButtonSize) / 2.0)), .roundingPower = 2.F});

        offset += scaledButtonsPad + scaledButtonSize;
    }
}

void CHyprBar::renderBarButtonsText(CBox* barBox, const float scale, const float a) {
    const float BUTTONSALPHA = g_pGlobalState->config.buttonsOnHover->value() ? m_fButtonsAlpha->value() : 1.F;
    if (BUTTONSALPHA <= 0.F)
        return;

    const auto HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const auto ICONONHOVER      = g_pGlobalState->config.iconOnHover->value();

    const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
    const auto visibleCount = getVisibleButtonCount(BARBUTTONPADDING, BARPADDING, Vector2D{barBox->w, barBox->h}, scale);
    const auto COORDS       = cursorRelativeToBar();
    const bool POPPING      = buttonsPopping();
    const auto CLIP         = POPPING && m_eButtonsPopStyle == BUTTONS_POP_SLIDE ? std::optional<CRegion>{buttonsPopClip(*barBox)} : std::nullopt;

    int        offset        = BARPADDING * scale;
    float      noScaleOffset = BARPADDING;

    if (g_pGlobalState->buttonIconScales.size() != g_pGlobalState->buttons.size())
        g_pGlobalState->buttonIconScales.resize(g_pGlobalState->buttons.size());

    for (size_t i = 0; i < visibleCount; ++i) {
        auto&      button           = g_pGlobalState->buttons[i];
        auto&      iconScale        = g_pGlobalState->buttonIconScales[i];
        const auto scaledButtonSize = button.size * scale;
        const auto scaledButtonsPad = BARBUTTONPADDING * scale;

        // check if hovering here
        const auto BARBUF     = Vector2D{(int)assignedBoxGlobal().w, HEIGHT};
        Vector2D   currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - BARBUTTONPADDING - button.size - noScaleOffset : noScaleOffset), (BARBUF.y - button.size) / 2.0}.floor();
        bool       hovering   = VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + button.size + BARBUTTONPADDING, currentPos.y + button.size);
        noScaleOffset += BARBUTTONPADDING + button.size;

        const bool NEEDICON = !button.icon.empty() && (!button.iconTex || button.iconTex->m_texID == 0 || !iconScale.has_value() ||
                                                      std::abs(iconScale.value_or(0.F) - scale) > 1e-6);
        if (NEEDICON) {
            // render icon
            auto fgcol = button.userfg ? button.fgcol : (button.bgcol.r + button.bgcol.g + button.bgcol.b < 1) ? CHyprColor(0xFFFFFFFF) : CHyprColor(0xFF000000);

            button.iconTex   = g_pHyprRenderer->renderText(button.icon, fgcol, std::round(button.size * 0.62 * scale), false, "sans", scaledButtonSize);
            iconScale        = scale;
        }

        if (!button.iconTex || button.iconTex->m_texID == 0)
            continue;

        const auto iconX = barBox->x + (BUTTONSRIGHT ? barBox->width - offset - scaledButtonSize / 2.0 : offset + scaledButtonSize / 2.0) - button.iconTex->m_size.x / 2.0;
        const auto iconY = barBox->y + barBox->height / 2.0 - button.iconTex->m_size.y / 2.0;
        CBox       pos   = {iconX, iconY, button.iconTex->m_size.x, button.iconTex->m_size.y};
        if (POPPING)
            pos = buttonsPopBox(pos, scaledButtonSize, BUTTONSRIGHT);

        if ((!ICONONHOVER || (ICONONHOVER && m_iButtonHoverState > 0)) && pos.w > 0 && pos.h > 0)
            g_pHyprOpenGL->renderTexture(button.iconTex, pos, {.damage = CLIP ? &*CLIP : nullptr, .a = a * BUTTONSALPHA});
        offset += scaledButtonsPad + scaledButtonSize;

        bool currentBit = (m_iButtonHoverState & (1 << i)) != 0;
        if (hovering != currentBit) {
            m_iButtonHoverState ^= (1 << i);
            // damage to get rid of some artifacts when icons are "hidden"
            damageEntire();
        }
    }
}

void CHyprBar::draw(PHLMONITOR pMonitor, const float& a) {
    const auto ENABLED = g_pGlobalState->config.enabled->value();

    if (m_bLastEnabledState != ENABLED) {
        m_bLastEnabledState = ENABLED;
        g_pDecorationPositioner->repositionDeco(this);
    }

    if (m_hidden || !validMapped(m_pWindow) || !ENABLED)
        return;

    const auto PWINDOW = m_pWindow.lock();

    if (!PWINDOW->m_ruleApplicator->decorate().valueOrDefault())
        return;

    auto data = CBarPassElement::SBarData{this, a};
    g_pHyprRenderer->m_renderPass.add(makeUnique<CBarPassElement>(data));
}

void CHyprBar::renderPass(PHLMONITOR pMonitor, const float& a) {
    const auto  PWINDOW = m_pWindow.lock();

    static auto PENABLEBLURGLOBAL = CConfigValue<Config::BOOL>("decoration:blur:enabled");
    const auto  BARCOLOR          = g_pGlobalState->config.barColor->value();
    const auto  HEIGHT            = g_pGlobalState->config.barHeight->value();
    const auto  PRECEDENCE        = g_pGlobalState->config.barPrecedenceOverBorder->value();
    const auto  ALIGNBUTTONS      = g_pGlobalState->config.barButtonsAlignment->value();
    const auto  ENABLETITLE       = g_pGlobalState->config.barTitleEnabled->value();
    const auto  ENABLEBLUR        = g_pGlobalState->config.barBlur->value();
    const auto  INACTIVECOLOR     = g_pGlobalState->config.inactiveButtonColor->value();

    if (INACTIVECOLOR > 0) {
        bool currentWindowFocus = PWINDOW == Desktop::focusState()->window();
        if (currentWindowFocus != m_bWindowHasFocus) {
            m_bWindowHasFocus = currentWindowFocus;
            m_bButtonsDirty   = true;
        }
    }

    const CHyprColor DEST_COLOR = m_bForcedBarColor.value_or(configColor(BARCOLOR));
    if (DEST_COLOR != m_cRealBarColor->goal())
        *m_cRealBarColor = DEST_COLOR;

    CHyprColor color = m_cRealBarColor->value();

    color.a *= a;
    const bool BUTTONSRIGHT = ALIGNBUTTONS != "left";
    const bool SHOULDBLUR   = ENABLEBLUR && *PENABLEBLURGLOBAL && color.a < 1.F;

    if (HEIGHT < 1) {
        m_iLastHeight = HEIGHT;
        return;
    }

    const auto PWORKSPACE      = PWINDOW->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !PWINDOW->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    const auto ROUNDING = PWINDOW->rounding() + (PRECEDENCE ? 0 : PWINDOW->getRealBorderSize());

    const auto scaledRounding = ROUNDING > 0 ? ROUNDING * pMonitor->m_scale - 2 /* idk why but otherwise it looks bad due to the gaps */ : 0;

    m_seExtents = {{0, HEIGHT}, {}};

    const auto DECOBOX = assignedBoxGlobal();

    const auto BARBUF = DECOBOX.size() * pMonitor->m_scale;

    CBox       titleBarBox = {DECOBOX.x - pMonitor->m_position.x, DECOBOX.y - pMonitor->m_position.y, DECOBOX.w,
                              DECOBOX.h + ROUNDING * 3 /* to fill the bottom cuz we can't disable rounding there */};

    titleBarBox.translate(PWINDOW->m_floatingOffset).scale(pMonitor->m_scale).round();

    if (titleBarBox.w < 1 || titleBarBox.h < 1)
        return;

    g_pHyprOpenGL->scissor(titleBarBox);

    if (ROUNDING) {
        // the +1 is a shit garbage temp fix until renderRect supports an alpha matte
        CBox windowBox = {PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x + PWINDOW->m_floatingOffset.x - pMonitor->m_position.x + 1,
                          PWINDOW->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y + PWINDOW->m_floatingOffset.y - pMonitor->m_position.y + 1,
                          PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).x - 2, PWINDOW->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT).y - 2};

        if (windowBox.w < 1 || windowBox.h < 1)
            return;

        glStencilMask(0xFF);

        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);

        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, true);

        glStencilFunc(GL_ALWAYS, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

        glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

        windowBox.translate(WORKSPACEOFFSET).scale(pMonitor->m_scale).round();
        g_pHyprOpenGL->renderRect(windowBox, CHyprColor(0, 0, 0, 0), {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

        glStencilFunc(GL_NOTEQUAL, 1, -1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    }

    if (SHOULDBLUR)
        g_pHyprOpenGL->renderRect(titleBarBox, color, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower(), .blur = true, .blurA = a});
    else
        g_pHyprOpenGL->renderRect(titleBarBox, color, {.round = scaledRounding, .roundingPower = m_pWindow->roundingPower()});

    // render title
    const int SCALEDTEXTSIZE = std::round(g_pGlobalState->config.barTextSize->value() * pMonitor->m_scale);
    if (ENABLETITLE && (m_szLastTitle != PWINDOW->m_title || m_bWindowSizeChanged || !m_pTextTex || m_pTextTex->m_texID == 0 || m_bTitleColorChanged ||
                        m_iLastScaledTextSize != SCALEDTEXTSIZE)) {
        m_szLastTitle = PWINDOW->m_title;
        renderBarTitle(BARBUF, pMonitor->m_scale);
        m_iLastScaledTextSize = SCALEDTEXTSIZE;
    }

    if (ROUNDING) {
        // cleanup stencil
        glStencilMask(-1);
        glClearStencil(0);
        glClear(GL_STENCIL_BUFFER_BIT);
        g_pHyprOpenGL->setCapStatus(GL_STENCIL_TEST, false);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
    }

    CBox textBox = {titleBarBox.x, titleBarBox.y, (int)BARBUF.x, (int)BARBUF.y};
    if (ENABLETITLE && m_pTextTex) {
        const auto BARPADDING       = g_pGlobalState->config.barPadding->value();
        const auto BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
        const auto ALIGN            = g_pGlobalState->config.barTextAlign->value();

        float      buttonSizes = BARBUTTONPADDING;
        for (auto& b : g_pGlobalState->buttons) {
            buttonSizes += b.size + BARBUTTONPADDING;
        }

        const auto scaledBorderSize  = PWINDOW->getRealBorderSize() * pMonitor->m_scale;
        const auto scaledButtonsSize = buttonSizes * pMonitor->m_scale;
        const auto scaledBarPadding  = BARPADDING * pMonitor->m_scale;
        const auto xOffset           = ALIGN == "left" ? std::round(scaledBarPadding + (BUTTONSRIGHT ? 0 : scaledButtonsSize)) :
                                                         std::round(((BARBUF.x - scaledBorderSize) / 2.0 - m_pTextTex->m_size.x / 2.0));
        const auto yOffset           = std::round((BARBUF.y - m_pTextTex->m_size.y) / 2.0);
        CBox       titleBox          = {textBox.x + xOffset, textBox.y + yOffset, m_pTextTex->m_size.x, m_pTextTex->m_size.y};

        g_pHyprOpenGL->renderTexture(m_pTextTex, titleBox, {.a = a});
    }

    renderBarButtons(&textBox, pMonitor->m_scale, a);
    m_bButtonsDirty = false;

    g_pHyprOpenGL->scissor(nullptr);

    renderBarButtonsText(&textBox, pMonitor->m_scale, a);

    m_bWindowSizeChanged = false;
    m_bTitleColorChanged = false;

    // dynamic updates change the extents
    if (m_iLastHeight != HEIGHT) {
        PWINDOW->layoutTarget()->recalc();
        m_iLastHeight = HEIGHT;
    }
}

eDecorationType CHyprBar::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CHyprBar::updateWindow(PHLWINDOW pWindow) {
    damageEntire();
}

void CHyprBar::onConfigReloaded() {
    m_bButtonsDirty      = true;
    m_bTitleColorChanged = true;
    m_pTextTex           = nullptr;

    g_pDecorationPositioner->repositionDeco(this);
    damageEntire();
}

void CHyprBar::damageEntire() {
    g_pHyprRenderer->damageBox(assignedBoxGlobal());
}

Vector2D CHyprBar::cursorRelativeToBar() {
    return g_pInputManager->getMouseCoordsInternal() - assignedBoxGlobal().pos();
}

eDecorationLayer CHyprBar::getDecorationLayer() {
    return DECORATION_LAYER_UNDER;
}

uint64_t CHyprBar::getDecorationFlags() {
    return DECORATION_ALLOWS_MOUSE_INPUT | (g_pGlobalState->config.barPartOfWindow->value() ? DECORATION_PART_OF_MAIN_WINDOW : 0);
}

CBox CHyprBar::assignedBoxGlobal() {
    if (!validMapped(m_pWindow))
        return {};

    CBox box = m_bAssignedBox;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_pWindow.lock()));

    const auto PWORKSPACE      = m_pWindow->m_workspace;
    const auto WORKSPACEOFFSET = PWORKSPACE && !m_pWindow->m_pinned ? PWORKSPACE->m_renderOffset->value() : Vector2D();

    return box.translate(WORKSPACEOFFSET);
}

PHLWINDOW CHyprBar::getOwner() {
    return m_pWindow.lock();
}

void CHyprBar::updateRules() {
    const auto PWINDOW              = m_pWindow.lock();
    auto       prevHidden           = m_hidden;
    auto       prevForcedTitleColor = m_bForcedTitleColor;

    m_bForcedBarColor   = std::nullopt;
    m_bForcedTitleColor = std::nullopt;
    m_hidden            = false;

    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->nobarRuleIdx))
        m_hidden = truthy(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->nobarRuleIdx)->effect);
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->barColorRuleIdx))
        m_bForcedBarColor = CHyprColor(Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->barColorRuleIdx)->effect).value_or(0));
    if (PWINDOW->m_ruleApplicator->m_otherProps.props.contains(g_pGlobalState->titleColorRuleIdx))
        m_bForcedTitleColor = CHyprColor(Config::ParserUtils::parseColor(PWINDOW->m_ruleApplicator->m_otherProps.props.at(g_pGlobalState->titleColorRuleIdx)->effect).value_or(0));

    if (prevHidden != m_hidden)
        g_pDecorationPositioner->repositionDeco(this);
    if (prevForcedTitleColor != m_bForcedTitleColor)
        m_bTitleColorChanged = true;
}

unsigned int CHyprBar::hoveredButtons() {
    const auto   BARPADDING       = g_pGlobalState->config.barPadding->value();
    const auto   BARBUTTONPADDING = g_pGlobalState->config.barButtonPadding->value();
    const auto   HEIGHT           = g_pGlobalState->config.barHeight->value();
    const auto   ALIGNBUTTONS     = g_pGlobalState->config.barButtonsAlignment->value();
    const bool   BUTTONSRIGHT     = ALIGNBUTTONS != "left";

    const auto   COORDS = cursorRelativeToBar();
    const auto   BARBUF = Vector2D{static_cast<double>(static_cast<int>(assignedBoxGlobal().w)), static_cast<double>(HEIGHT)};

    float        offset  = BARPADDING;
    unsigned int hovered = 0;

    for (size_t i = 0; i < g_pGlobalState->buttons.size() && i < 32; ++i) {
        const auto& b          = g_pGlobalState->buttons[i];
        const auto  currentPos = Vector2D{(BUTTONSRIGHT ? BARBUF.x - BARBUTTONPADDING - b.size - offset : offset), (BARBUF.y - b.size) / 2.0}.floor();

        if (VECINRECT(COORDS, currentPos.x, currentPos.y, currentPos.x + b.size + BARBUTTONPADDING, currentPos.y + b.size))
            hovered |= 1U << i;

        offset += BARBUTTONPADDING + b.size;
    }

    return hovered;
}

void CHyprBar::damageOnButtonHover() {
    const auto HOVERED = hoveredButtons();

    if (HOVERED != m_iButtonHoverDamageState) {
        m_iButtonHoverDamageState = HOVERED;
        damageEntire();
    }
}

bool CHyprBar::buttonsPopping() {
    return g_pGlobalState->config.buttonsOnHover->value() && g_pGlobalState->config.buttonsPopIn->value() && m_fButtonsPop->value() != 1.F;
}

void CHyprBar::setButtonsPopStyle(std::string style) {
    std::ranges::transform(style, style.begin(), [](unsigned char c) { return std::tolower(c); });

    // Read the way Hyprland reads a window's style, so "popin 60%" means the
    // same here. Anything that is not a slide pops in, gnomed included, since
    // that is where windows fall back too; with no size named it starts at 80%.
    m_eButtonsPopStyle    = style.starts_with("slide") ? BUTTONS_POP_SLIDE : BUTTONS_POP_POPIN;
    m_fButtonsPopMinScale = 0.8F;

    if (m_eButtonsPopStyle != BUTTONS_POP_POPIN || style.find('%') == std::string::npos)
        return;

    try {
        m_fButtonsPopMinScale = std::clamp(std::stoi(style.substr(style.find_last_of(' ') + 1)) / 100.F, 0.F, 1.F);
    } catch (...) { ; }
}

CBox CHyprBar::buttonsPopBox(CBox box, const float distance, const bool buttonsRight) {
    const float PROGRESS = m_fButtonsPop->value();

    // In from the edge the buttons are aligned to, by about a button's width:
    // enough to read as movement in a bar this short.
    if (m_eButtonsPopStyle == BUTTONS_POP_SLIDE)
        return box.translate({(buttonsRight ? 1.F : -1.F) * (1.F - PROGRESS) * distance, 0.F});

    // An overshooting curve can dip below zero on the way in.
    return box.scaleFromCenter(std::max(0.F, m_fButtonsPopMinScale + (1.F - m_fButtonsPopMinScale) * PROGRESS));
}

CRegion CHyprBar::buttonsPopClip(const CBox& barBox) {
    // A button that starts past the bar's edge would leave pixels where the
    // bar never damages again, so nothing may be drawn outside the bar.
    CRegion clip{barBox};
    g_pHyprRenderer->m_renderData.renderModif.applyToRegion(clip);
    clip.intersect(g_pHyprRenderer->m_renderData.damage);
    return clip;
}
