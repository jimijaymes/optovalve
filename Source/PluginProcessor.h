#pragma once
#include <JuceHeader.h>
#include <array>
#include <atomic>

class OptoValveProcessor final : public juce::AudioProcessor
{
public:
    OptoValveProcessor();
    void prepareToPlay(double, int) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }
    const juce::String getName() const override { return "OptoValve 1.5"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override;
    void setStateInformation(const void*, int) override;
    std::atomic<float> gainReductionDb { 0.0f };
    juce::AudioProcessorValueTreeState parameters;
private:
    static juce::AudioProcessorValueTreeState::ParameterLayout makeLayout();
    struct DetectorHP { float x1=0, y1=0; float process(float x, float a) { float y=a*(y1+x-x1); x1=x; y1=y; return y; } };
    std::array<DetectorHP,2> detector;
    // Magnetic-flux proxy and frequency-dependent core response, per channel.
    std::array<float,2> transformerFlux {0,0};
    std::array<float,2> transformerBass {0,0};
    std::array<float,2> transformerTreble {0,0};
    std::array<float,2> outputCoreFlux {0,0}, outputCoreBass {0,0}, outputCoreTreble {0,0};
    std::array<float,2> tubeBiasMemory {0,0}, tubeCathodeMemory {0,0};
    std::array<float,2> vintageHighCutState {0,0};
    float vintageHighCutBlend=0.0f;
    // Frequency-sensitive optical detector with independent bass and presence envelopes.
    std::array<float,2> detectorBass {0,0};
    std::array<float,2> sidechainEmphasisLow {0,0};
    float bassEnvelope=0.0f, presenceEnvelope=0.0f;
    float envelopeDb=0.0f, slowMemory=0.0f, smoothedGR=0.0f;
    float t4FastCell=0.0f, t4SlowCell=0.0f, t4Exposure=0.0f;
    float t4GainReduction=0.0f, responseBlend=0.0f;
    std::unique_ptr<juce::dsp::Oversampling<float>> colourOversampling;
    juce::AudioBuffer<float> colourBuffer, dryBuffer;
    std::array<std::vector<float>, 2> dryDelay;
    size_t dryDelayPosition = 0;
    int dryLatency = 0;
    int preparedBlockSize = 512;
    double sampleRateHz=44100.0;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(OptoValveProcessor)
};
