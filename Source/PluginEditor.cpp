#include "PluginEditor.h"
#include <cmath>

namespace {
const juce::Colour cream(0xffe8dcc0), ink(0xff252b2b), gold(0xffb59a62);
}

void OptoValveEditor::DialLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int w, int h,
    float position, float startAngle, float endAngle, juce::Slider&)
{
    const auto bounds = juce::Rectangle<float>((float)x, (float)y, (float)w, (float)h).reduced(9.0f);
    const auto diameter = juce::jmin(bounds.getWidth(), bounds.getHeight());
    const auto centre = bounds.getCentre();
    const float r = diameter * 0.43f;
    g.setColour(juce::Colour(0xff0f1516));
    g.fillEllipse(centre.x-r-3, centre.y-r-3, 2*r+6, 2*r+6);
    g.setColour(juce::Colour(0xffd1c7b3));
    g.fillEllipse(centre.x-r, centre.y-r, 2*r, 2*r);
    g.setColour(juce::Colour(0xff6c6860));
    g.drawEllipse(centre.x-r, centre.y-r, 2*r, 2*r, 1.5f);
    const float angle = startAngle + position*(endAngle-startAngle);
    juce::Path pointer;
    pointer.addRoundedRectangle(-2.2f, -r+8.0f, 4.4f, r*0.58f, 1.8f);
    g.setColour(ink);
    g.fillPath(pointer, juce::AffineTransform::rotation(angle).translated(centre.x, centre.y));
    juce::Path arc;
    arc.addCentredArc(centre.x, centre.y, r+8, r+8, 0, startAngle, endAngle, true);
    g.setColour(gold.withAlpha(0.75f));
    g.strokePath(arc, juce::PathStrokeType(1.8f));
}

OptoValveEditor::OptoValveEditor(OptoValveProcessor& p) : AudioProcessorEditor(p), processor(p)
{
    const char* ids[] = {"input", "reduction", "output", "knee", "hpf", "tube", "transformer", "mix"};
    const char* names[] = {"INPUT TRIM", "PEAK REDUCTION", "GAIN", "KNEE", "SC HIGH PASS", "TUBE DRIVE", "TRANSFORMER", "MIX"};
    for (size_t i=0; i<knobs.size(); ++i) {
        auto& k=knobs[i];
        k.slider.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
        k.slider.setTextBoxStyle(juce::Slider::TextBoxBelow, false, 100, 21);
        k.slider.setLookAndFeel(&dialLook);
        k.slider.setColour(juce::Slider::textBoxTextColourId, cream);
        k.slider.setColour(juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        k.slider.setColour(juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(k.slider);
        k.label.setText(names[i], juce::dontSendNotification);
        k.label.setJustificationType(juce::Justification::centred);
        k.label.setColour(juce::Label::textColourId, cream);
        k.label.setFont(juce::Font(juce::FontOptions(12.0f, juce::Font::bold)));
        addAndMakeVisible(k.label);
        k.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(p.parameters, ids[i], k.slider);
    }
    mode.addItem("COMPRESS 4:1",1);
    mode.addItem("LIMIT 10:1",2);
    mode.setColour(juce::ComboBox::backgroundColourId, ink);
    mode.setColour(juce::ComboBox::textColourId, cream);
    mode.setColour(juce::ComboBox::outlineColourId, gold);
    addAndMakeVisible(mode);
    modeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.parameters,"mode",mode);
    modeLabel.setText("OPERATING MODE",juce::dontSendNotification);
    modeLabel.setJustificationType(juce::Justification::centred);
    modeLabel.setColour(juce::Label::textColourId,cream);
    addAndMakeVisible(modeLabel);
    releaseStyle.addItem("LA-2A  SLOW",1);
    releaseStyle.addItem("LA-3A  FAST",2);
    releaseStyle.setColour(juce::ComboBox::backgroundColourId,ink);
    releaseStyle.setColour(juce::ComboBox::textColourId,cream);
    releaseStyle.setColour(juce::ComboBox::outlineColourId,gold);
    addAndMakeVisible(releaseStyle);
    releaseAttachment=std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(p.parameters,"releaseStyle",releaseStyle);
    releaseLabel.setText("OPTICAL RELEASE",juce::dontSendNotification);
    releaseLabel.setJustificationType(juce::Justification::centred);
    releaseLabel.setColour(juce::Label::textColourId,cream);
    addAndMakeVisible(releaseLabel);
    setSize(1030, 520);
    startTimerHz(30);
}

void OptoValveEditor::paint(juce::Graphics& g)
{
    g.fillAll(juce::Colour(0xff202728));
    g.setColour(juce::Colour(0xff343b3b));
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(11.0f), 12.0f);
    g.setColour(gold);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(11.0f),12.0f,2.0f);
    g.setColour(cream);
    g.setFont(juce::Font(juce::FontOptions(33.0f,juce::Font::bold)));
    g.drawText("OPTO  VALVE  1.1",35,22,500,48,juce::Justification::centredLeft);
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText("PROGRAM DEPENDENT OPTICAL TUBE COMPRESSOR",38,68,460,22,juce::Justification::centredLeft);
    g.setColour(gold);
    g.drawLine(32.0f,105.0f,998.0f,105.0f,2.0f);
    const auto m=meterArea.toFloat();
    g.setColour(juce::Colour(0xff151a1a));
    g.fillRoundedRectangle(m,9.0f);
    g.setColour(gold);
    g.drawRoundedRectangle(m,9.0f,2.0f);
    auto face=m.reduced(12.0f);
    g.setColour(juce::Colour(0xffe6d6ae));
    g.fillRoundedRectangle(face,6.0f);
    g.setColour(ink);
    g.setFont(juce::Font(juce::FontOptions(13.0f,juce::Font::bold)));
    g.drawText("GAIN REDUCTION  •  dB",face.removeFromTop(25.0f),juce::Justification::centred);
    const auto c=m.getCentre();
    const float radius=juce::jmin(m.getWidth()*0.39f,m.getHeight()*0.55f);
    for(int i=0;i<=8;++i) {
        const float a=juce::degreesToRadians(65.0f-130.0f*i/8.0f);
        const float sx=c.x+std::sin(a)*radius*0.70f;
        const float sy=c.y+std::cos(a)*radius*0.70f;
        const float ex=c.x+std::sin(a)*radius*0.85f;
        const float ey=c.y+std::cos(a)*radius*0.85f;
        g.drawLine(sx,sy,ex,ey,1.4f);
    }
    const float value=juce::jlimit(0.0f,30.0f,displayedReduction);
    const float angle=juce::degreesToRadians(65.0f-130.0f*(value/30.0f));
    g.setColour(juce::Colour(0xffae4c36));
    g.drawLine(c.x,c.y+24.0f,c.x+std::sin(angle)*radius*0.84f,c.y+std::cos(angle)*radius*0.84f,2.8f);
    g.setColour(ink);
    g.fillEllipse(c.x-6,c.y+18,12,12);
    g.setColour(ink);
    g.setFont(juce::Font(juce::FontOptions(11.0f,juce::Font::bold)));
    g.drawText("0", (int)(c.x+radius*0.70f)-8,(int)(c.y+radius*0.31f)-8,22,18,juce::Justification::centred);
    g.drawText("30", (int)(c.x-radius*0.70f)-8,(int)(c.y+radius*0.31f)-8,28,18,juce::Justification::centred);
    g.setColour(cream);
    g.setFont(juce::Font(juce::FontOptions(11.0f)));
    g.drawText("OPTICAL LEVELING AMPLIFIER   •   ORIGINAL DSP DESIGN",30,484,620,20,juce::Justification::centredLeft);
}

void OptoValveEditor::resized()
{
    meterArea={775,20,225,155};
    // Four knobs per row; the input trim sits first in the signal-flow layout.
    for(int i=0;i<8;++i) {
        const int x=32+(i%4)*182;
        const int y=135+(i/4)*167;
        knobs[(size_t)i].slider.setBounds(x,y,136,113);
        knobs[(size_t)i].label.setBounds(x-5,y+110,146,25);
    }
    modeLabel.setBounds(784,192,210,22);
    mode.setBounds(798,218,185,30);
    releaseLabel.setBounds(784,266,210,22);
    releaseStyle.setBounds(798,292,185,30);
}

void OptoValveEditor::timerCallback()
{
    const auto next=processor.gainReductionDb.load(std::memory_order_relaxed);
    displayedReduction += 0.22f*(next-displayedReduction);
    repaint(meterArea);
}
