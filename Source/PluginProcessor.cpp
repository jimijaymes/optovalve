#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

namespace {
float dbToGain(float db) { return juce::Decibels::decibelsToGain(db); }
float sat(float x) { return std::tanh(x); }
float coeff(float seconds, double sr) { return std::exp(-1.0f / juce::jmax(1.0f, seconds * (float)sr)); }
}

juce::AudioProcessorValueTreeState::ParameterLayout OptoValveProcessor::makeLayout()
{
    using APF=juce::AudioParameterFloat;
    using APC=juce::AudioParameterChoice;
    juce::AudioProcessorValueTreeState::ParameterLayout p;
    p.add(std::make_unique<APF>(juce::ParameterID{"input",1}, "Input Trim (dB)", juce::NormalisableRange<float>(-24.0f,12.0f,0.1f), 0.0f));
    p.add(std::make_unique<APF>(juce::ParameterID{"reduction",1}, "Peak Reduction", juce::NormalisableRange<float>(0,60,0.1f), 25));
    p.add(std::make_unique<APF>(juce::ParameterID{"output",1}, "Output Gain (dB)", juce::NormalisableRange<float>(-24,24,0.1f), 0));
    p.add(std::make_unique<APC>(juce::ParameterID{"mode",1}, "Mode", juce::StringArray{"Compress 4:1", "Limit 10:1"}, 0));
    p.add(std::make_unique<APF>(juce::ParameterID{"knee",1}, "Knee (dB)", juce::NormalisableRange<float>(0,24,0.1f), 12));
    p.add(std::make_unique<APF>(juce::ParameterID{"hpf",1}, "Sidechain HPF (Hz)", juce::NormalisableRange<float>(20,500,1,0.4f), 90));
    p.add(std::make_unique<APF>(juce::ParameterID{"tube",1}, "Tube Drive", juce::NormalisableRange<float>(0,100,0.1f), 25));
    p.add(std::make_unique<APF>(juce::ParameterID{"transformer",1}, "Transformer Drive", juce::NormalisableRange<float>(0,100,0.1f), 20));
    p.add(std::make_unique<APC>(juce::ParameterID{"releaseStyle",1}, "Release Style", juce::StringArray{"LA-2A Slow", "LA-3A Fast"}, 0));
    p.add(std::make_unique<APF>(juce::ParameterID{"mix",1}, "Wet Mix (%)", juce::NormalisableRange<float>(0,100,0.1f), 100));
    return p;
}
OptoValveProcessor::OptoValveProcessor() : AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true).withOutput("Output",juce::AudioChannelSet::stereo(),true)), parameters(*this,nullptr,"STATE",makeLayout()) {}
void OptoValveProcessor::prepareToPlay(double sr, int maximumExpectedSamplesPerBlock)
{
    sampleRateHz=sr; detector={}; transformerMemory={0,0}; envelopeDb=slowMemory=smoothedGR=0; gainReductionDb.store(0);
    preparedBlockSize = juce::jmax(1, maximumExpectedSamplesPerBlock);
    colourBuffer.setSize(2, preparedBlockSize);
    dryBuffer.setSize(2, preparedBlockSize);
    colourOversampling = std::make_unique<juce::dsp::Oversampling<float>>(
        2, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
    colourOversampling->initProcessing(static_cast<size_t>(preparedBlockSize));
    colourOversampling->reset();
    dryLatency = static_cast<int>(std::round(colourOversampling->getLatencyInSamples()));
    for(auto& d:dryDelay) d.assign(static_cast<size_t>(juce::jmax(1,dryLatency+1)),0.0f);
    dryDelayPosition=0;
    setLatencySamples(dryLatency);
}
void OptoValveProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int n=buffer.getNumSamples(), channels=juce::jmin(2,buffer.getNumChannels());
    if(channels==0) return;
    for(int c=channels;c<buffer.getNumChannels();++c) buffer.clear(c,0,n);
    const float inputGain=dbToGain(parameters.getRawParameterValue("input")->load(std::memory_order_relaxed));
    const float reduction=*parameters.getRawParameterValue("reduction");
    const float makeup=dbToGain(*parameters.getRawParameterValue("output"));
    const float ratio=*parameters.getRawParameterValue("mode") < 0.5f ? 4.0f : 10.0f;
    const float knee=*parameters.getRawParameterValue("knee");
    const float cutoff=*parameters.getRawParameterValue("hpf");
    const float tube=*parameters.getRawParameterValue("tube")*0.01f;
    const float xfmr=*parameters.getRawParameterValue("transformer")*0.01f;
    const float mix=*parameters.getRawParameterValue("mix")*0.01f;
    const bool fastMode=*parameters.getRawParameterValue("releaseStyle")>0.5f;
    const float hpA=1.0f/(1.0f+2.0f*juce::MathConstants<float>::pi*cutoff/(float)sampleRateHz);
    // LA-2A nominal attack is approximately 10 ms; measured response is program-dependent.
    const float attack=coeff(0.010f,sampleRateHz);
    const float fastRelease=coeff(fastMode ? 0.045f : 0.075f,sampleRateHz);
    const float slowRelease=coeff(fastMode ? 0.35f : 1.6f,sampleRateHz);
    const float memoryRelease=coeff(fastMode ? 0.65f : 2.8f,sampleRateHz);
    // Keep the secondary smoothing short so it does not noticeably slow the 10 ms attack.
    const float grSmooth=coeff(0.0005f,sampleRateHz);
    const float slope=1.0f-1.0f/ratio;
    // Reduction maps to an approximate optical detector threshold; no fixed 1:1 hardware calibration.
    const float threshold=-reduction;
    const float transformerInputMemoryCoef=coeff(0.012f,sampleRateHz);
    const float transformerDrive=1.0f+1.5f*xfmr;
    const float transformerNorm=transformerDrive;
    // The dry delay compensates for the oversampler's host-rate group delay.
    // Store the trimmed signal before gain reduction is applied.
    if(dryBuffer.getNumSamples()<n) dryBuffer.setSize(2,n,false,false,true);
    for(int i=0;i<n;++i)
    {
        float level=0;
        for(int c=0;c<channels;++c) {
            const float original=buffer.getReadPointer(c)[i]*inputGain;
            auto& mem=transformerMemory[(size_t)c];
            mem=transformerInputMemoryCoef*mem+(1.0f-transformerInputMemoryCoef)*original;
            const float transformed=xfmr>0.0001f ? std::tanh((original+0.12f*xfmr*mem)*transformerDrive)/transformerNorm : original;
            buffer.getWritePointer(c)[i]=transformed;
            level=juce::jmax(level,std::abs(detector[(size_t)c].process(transformed,hpA)));
        }
        const float levelDb=juce::Decibels::gainToDecibels(juce::jmax(level,1.0e-8f),-160.0f);
        const float above=levelDb-threshold;
        float target=0;
        if(knee<0.01f) target=juce::jmax(0.0f,above)*slope;
        else if(above<=-knee*0.5f) target=0;
        else if(above>=knee*0.5f) target=above*slope;
        else { const float t=above+knee*0.5f; target=slope*t*t/(2.0f*knee); }
        // Fast initial recovery, then a longer memory tail for sustained compression.
        slowMemory=juce::jmax(target,slowMemory*memoryRelease);
        const float tailWeight=juce::jlimit(0.0f,0.85f,slowMemory/32.0f);
        const float release=fastRelease+(slowRelease-fastRelease)*tailWeight;
        const float envCoef=target>envelopeDb?attack:release;
        envelopeDb=envCoef*envelopeDb+(1.0f-envCoef)*target;
        smoothedGR=grSmooth*smoothedGR+(1.0f-grSmooth)*envelopeDb;
        const float gain=dbToGain(-smoothedGR);
        for(int c=0;c<channels;++c)
        {
            const float original=buffer.getReadPointer(c)[i];
            const auto pos=dryDelayPosition;
            auto& ring=dryDelay[static_cast<size_t>(c)];
            const float delayed=ring[pos];
            ring[pos]=original;
            dryBuffer.getWritePointer(c)[i]=dryLatency==0 ? original : delayed;
            buffer.getWritePointer(c)[i]=original*gain;
        }
        dryDelayPosition=(dryDelayPosition+1)%dryDelay[0].size();
    }
    // Nonlinear stages are evaluated at 4x the host sample rate to reduce aliasing.
    // Process in preallocated chunks so unexpectedly large host blocks do not allocate.
    // Transformer colour is applied before the compression detector.
    for(int offset=0;offset<n;offset+=preparedBlockSize)
    {
        const int count=juce::jmin(preparedBlockSize,n-offset);
        for(int c=0;c<2;++c)
        {
            const int sourceChannel=juce::jmin(c,channels-1);
            juce::FloatVectorOperations::copy(colourBuffer.getWritePointer(c),
                buffer.getReadPointer(sourceChannel,offset),count);
        }
        juce::dsp::AudioBlock<float> inputBlock(colourBuffer);
        auto inputSlice=inputBlock.getSubBlock(0,static_cast<size_t>(count));
        auto upBlock=colourOversampling->processSamplesUp(inputSlice);
        const int upCount=static_cast<int>(upBlock.getNumSamples());
        for(int c=0;c<2;++c)
        {
            float* samples=upBlock.getChannelPointer(static_cast<size_t>(c));
            float& mem=transformerMemory[static_cast<size_t>(c)];
            for(int j=0;j<upCount;++j)
            {
                float wet=samples[j];
                if(tube>0.0001f)
                {
                    const float drive=1.0f+1.5f*tube;
                    const float bias=0.08f*tube;
                    // Unity small-signal gain; soft saturation grows gradually with drive.
                    const float nonlinear=(sat(wet*drive+bias)-sat(bias));
                    const float smallSignal=1.0f-sat(bias)*sat(bias);
                    wet=nonlinear/(drive*juce::jmax(0.15f,smallSignal));
                }
                samples[j]=wet;
            }
        }
        colourOversampling->processSamplesDown(inputSlice);
        for(int c=0;c<channels;++c)
        {
            float* dest=buffer.getWritePointer(c,offset);
            const float* wet=colourBuffer.getReadPointer(c);
            for(int j=0;j<count;++j)
            {
                // Original dry path must be delayed to match oversampling latency.
                // The colour path is always active, including with zero drive.
                dest[j]=wet[j]*makeup*mix + dryBuffer.getReadPointer(c,offset)[j]*(1.0f-mix);
            }
        }
    }
    gainReductionDb.store(juce::jmax(0.0f,smoothedGR),std::memory_order_relaxed);
}
juce::AudioProcessorEditor* OptoValveProcessor::createEditor() { return new OptoValveEditor(*this); }
void OptoValveProcessor::getStateInformation(juce::MemoryBlock& b) { auto xml=parameters.copyState().createXml(); copyXmlToBinary(*xml,b); }
void OptoValveProcessor::setStateInformation(const void* data,int bytes) { if(auto xml=getXmlFromBinary(data,bytes)) if(xml->hasTagName(parameters.state.getType())) parameters.replaceState(juce::ValueTree::fromXml(*xml)); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OptoValveProcessor(); }
