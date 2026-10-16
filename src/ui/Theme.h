#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/Theory.h"

// Visual language for Chordbloom. Dark slate UI that sits comfortably next to FL Studio's blue-grey,
// soft rounded surfaces, one accent colour per harmonic function.
namespace cb::ui
{
namespace Colours
{
    inline const juce::Colour background   { 0xff1a1f26 };   // editor background
    inline const juce::Colour panel        { 0xff222932 };   // section panels
    inline const juce::Colour panelRaised  { 0xff2a323c };   // pads, chips, inputs
    inline const juce::Colour panelHover   { 0xff323b47 };
    inline const juce::Colour outline      { 0xff38424e };
    inline const juce::Colour text         { 0xffe9eef3 };
    inline const juce::Colour textDim      { 0xff93a1af };
    inline const juce::Colour textFaint    { 0xff5f6c79 };
    inline const juce::Colour accent       { 0xff7ad3b8 };   // brand mint (playhead, focus, primary buttons)

    inline const juce::Colour tonic        { 0xff58c9a9 };   // teal
    inline const juce::Colour subdominant  { 0xfff1b562 };   // amber
    inline const juce::Colour dominant     { 0xfff0766f };   // coral
    inline const juce::Colour borrowed     { 0xffaa8cf2 };   // lavender
    inline const juce::Colour secondary    { 0xff62a8f2 };   // sky
    inline const juce::Colour chromatic    { 0xff9aa5b1 };   // grey
}

juce::Colour functionColour (cb::HarmonicFunction);

// Inter from embedded binary data. weight: 400 (regular), 600 (semibold), 700 (bold). Falls back to the system sans.
juce::Font font (float height, int weight = 400);

constexpr float kCornerRadius = 8.0f;

//==============================================================================
// Shared drawing helpers (added by ui-shell; optional for other components).

// Rounded section surface: subtle vertical gradient, faint top highlight, 1 px low-contrast outline.
void drawPanel (juce::Graphics&, juce::Rectangle<float> bounds, float cornerRadius = kCornerRadius);

// Small uppercase section title: 10.5 px semibold, letter-spaced, dim colour.
void drawSectionTitle (juce::Graphics&, const juce::String& text, juce::Rectangle<int> area,
                       juce::Justification = juce::Justification::centredLeft);

// Pixel width of a single line of text in the given font.
float textWidth (const juce::Font&, const juce::String&);

// Component property flags understood by the LookAndFeel (set with component.getProperties().set (flag, true)):
//   ComboBox   "cb.prominent" -> larger semibold text (key / scale selectors)
//   TextButton "cb.chip"      -> small pill-shaped toggle chip (accent tint when on)
//   TextButton "cb.ghost"     -> no fill until hovered
inline constexpr const char* kPropProminent = "cb.prominent";
inline constexpr const char* kPropChip      = "cb.chip";
inline constexpr const char* kPropGhost     = "cb.ghost";

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    ~LookAndFeel() override;

    // Implementations live in Theme.cpp (combo boxes, popup menus, toggle buttons, text buttons, rotary + linear
    // sliders, labels, tooltips, scrollbars) - all restyled to the palette above.
    juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

    // Combo boxes
    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown, int buttonX, int buttonY,
                       int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;
    void drawComboBoxTextWhenNothingSelected (juce::Graphics&, juce::ComboBox&, juce::Label&) override;

    // Popup menus
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;
    void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon,
                            const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>& area,
                                     const juce::String& sectionName) override;
    void getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;
    void getIdealPopupMenuSectionHeaderSizeWithOptions (const juce::String& text, int standardMenuItemHeight,
                                                        int& idealWidth, int& idealHeight,
                                                        const juce::PopupMenu::Options&) override;
    juce::Font getPopupMenuFont() override;
    int getPopupMenuBorderSize() override;
    int getMenuWindowFlags() override;

    // Buttons
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool shouldDrawButtonAsHighlighted,
                         bool shouldDrawButtonAsDown) override;
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool shouldDrawButtonAsHighlighted,
                           bool shouldDrawButtonAsDown) override;
    void changeToggleButtonWidthToFitText (juce::ToggleButton&) override;

    // Sliders
    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height, float sliderPosProportional,
                           float rotaryStartAngle, float rotaryEndAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height, float sliderPos,
                           float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override;
    juce::Font getSliderPopupFont (juce::Slider&) override;
    int getSliderPopupPlacement (juce::Slider&) override;
    void drawBubble (juce::Graphics&, juce::BubbleComponent&, const juce::Point<float>& tip,
                     const juce::Rectangle<float>& body) override;

    // Labels
    juce::Font getLabelFont (juce::Label&) override;

    // Tooltips
    juce::Rectangle<int> getTooltipBounds (const juce::String& tipText, juce::Point<int> screenPos,
                                           juce::Rectangle<int> parentArea) override;
    void drawTooltip (juce::Graphics&, const juce::String& text, int width, int height) override;

    // Scrollbars, resize corner, text editors
    int getDefaultScrollbarWidth() override;
    void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isScrollbarVertical, int thumbStartPosition, int thumbSize, bool isMouseOver,
                        bool isMouseDown) override;
    void drawCornerResizer (juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;
    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;
};

} // namespace cb::ui
