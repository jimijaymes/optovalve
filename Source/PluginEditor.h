#pragma once
#include "PluginProcessor.h"

class OptoValveEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit OptoValveEditor(OptoValveProcessor&);
    void paint(juce::Graphics&) override;
    void resized() override;
private:
    class DialLookAndFeel final : public juce::LookAndFeel_V4 {
    public:
        void drawRotarySlider(juce::Graphics&, int, int, int, int, float, float, float, juce::Slider&) override;
    } dialLook;
    struct Knob {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    OptoValveProcessor& processor;
    std::array<Knob,8> knobs;
    juce::ComboBox mode, releaseStyle;
    juce::ToggleButton lofiSwitch;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> lofiAttachment;
    juce::Label modeLabel, releaseLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> modeAttachment, releaseAttachment;
    juce::Rectangle<int> meterArea;
    float displayedReduction = 0.0f;
    void timerCallback() override;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OptoValveEditor)
};
