#pragma once

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FontWeightValue.hpp>

#include <optional>

inline HANDLE PHANDLE = nullptr;

// Scroll Overview reads the button list straight out of g_pGlobalState through
// its own copy of this layout, so a member added here makes it read and write
// the wrong bytes and abort the compositor. Anything more a button needs goes
// in SGlobalState instead, beside the list.
struct SHyprButton {
    std::string          cmd     = "";
    bool                 userfg  = false;
    CHyprColor           fgcol   = CHyprColor(0, 0, 0, 0);
    CHyprColor           bgcol   = CHyprColor(0, 0, 0, 0);
    float                size    = 10;
    std::string          icon    = "";
    SP<Render::ITexture> iconTex;
};

class CHyprBar;

struct SGlobalState {
    std::vector<SHyprButton>  buttons;
    // The scale each button's icon texture was rendered at, by button index.
    std::vector<std::optional<float>> buttonIconScales;
    std::vector<WP<CHyprBar>> bars;
    uint32_t                  nobarRuleIdx      = 0;
    uint32_t                  barColorRuleIdx   = 0;
    uint32_t                  titleColorRuleIdx = 0;

    struct {
        SP<Config::Values::CColorValue>      barColor, textColor, inactiveButtonColor;
        SP<Config::Values::CIntValue>        barHeight;
        SP<Config::Values::CIntValue>        barTextSize;
        SP<Config::Values::CFontWeightValue> barTextWeight;
        SP<Config::Values::CIntValue>        barPadding;
        SP<Config::Values::CIntValue>        barButtonPadding;
        SP<Config::Values::CBoolValue>       barBlur, barTitleEnabled, barPartOfWindow, barPrecedenceOverBorder, enabled, iconOnHover, buttonsOnHover, buttonsPopIn;
        SP<Config::Values::CStringValue>     barTextFont, barTextAlign, barButtonsAlignment, onDoubleClick;
    } config;

    // The color each button's circle takes while the cursor is on it, by
    // button index. It lives out here, after everything else, because Scroll
    // Overview reads this state through its own copy of the layout.
    std::vector<std::optional<CHyprColor>> buttonHoverColors;

    // The title bar gestures, kept out of the config struct and after
    // everything else for the same reason.
    SP<Config::Values::CStringValue> onScrollUp, onScrollDown, onMiddleClick;

    // One scroll moves a window a single step. After a step nothing more
    // happens until vertical scrolling pauses, wherever the pointer has gone
    // by then, so a spinning wheel cannot skip steps and what is left of the
    // scroll reaches neither the bar nor the window that turns up under the
    // pointer next. A scroll that started somewhere else is disarmed too, but
    // it has moved nothing, so the rest of it still reaches what it is over;
    // `stepped` tells the two apart.
    struct {
        uint32_t lastAxisTimeMs = 0;
        double   accumulated    = 0.0;
        bool     armed          = true;
        bool     stepped        = false;
    } scrollGesture;
};

inline UP<SGlobalState> g_pGlobalState;
