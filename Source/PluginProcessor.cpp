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
    using APB=juce::AudioParameterBool;
    juce::AudioProcessorValueTreeState::ParameterLayout p;
    p.add(std::make_unique<APF>(juce::ParameterID{"input",1}, "Input Trim (dB)", juce::NormalisableRange<float>(-24.0f,12.0f,0.1f), 0.0f));
    p.add(std::make_unique<APF>(juce::ParameterID{"reduction",1}, "Peak Reduction", juce::NormalisableRange<float>(0,60,0.1f), 25));
    p.add(std::make_unique<APF>(juce::ParameterID{"output",1}, "Output Gain (dB)", juce::NormalisableRange<float>(-24,24,0.1f), 0));
    p.add(std::make_unique<APC>(juce::ParameterID{"mode",1}, "Mode", juce::StringArray{"Compress 4:1", "Limit 10:1"}, 0));
    p.add(std::make_unique<APF>(juce::ParameterID{"knee",1}, "Knee (dB)", juce::NormalisableRange<float>(0,24,0.1f), 12));
    p.add(std::make_unique<APF>(juce::ParameterID{"hpf",1}, "Sidechain HPF (Hz)", juce::NormalisableRange<float>(20,500,1,0.4f), 90));
    p.add(std::make_unique<APF>(juce::ParameterID{"scEmphasis",1}, "Sidechain Emphasis (%)", juce::NormalisableRange<float>(0,100,0.1f), 0));
    p.add(std::make_unique<APF>(juce::ParameterID{"tube",1}, "Tube Drive", juce::NormalisableRange<float>(0,100,0.1f), 25));
    p.add(std::make_unique<APF>(juce::ParameterID{"transformer",1}, "Transformer Drive", juce::NormalisableRange<float>(0,100,0.1f), 20));
    p.add(std::make_unique<APC>(juce::ParameterID{"releaseStyle",1}, "Release Style", juce::StringArray{"LA-2A Slow", "LA-3A Fast", "Smack"}, 0));
    p.add(std::make_unique<APC>(juce::ParameterID{"outputStage",1}, "Output Stage", juce::StringArray{"LA-2A Valve + Transformer", "1176 Class A + Transformer"}, 0));
    p.add(std::make_unique<APC>(juce::ParameterID{"response",1}, "Optical Response", juce::StringArray{"Enhanced", "Classic T4"}, 0));
    p.add(std::make_unique<APB>(juce::ParameterID{"lofi",1}, "Vintage Bandwidth", false));
    p.add(std::make_unique<APF>(juce::ParameterID{"mix",1}, "Wet Mix (%)", juce::NormalisableRange<float>(0,100,0.1f), 100));
    return p;
}
OptoValveProcessor::OptoValveProcessor() : AudioProcessor(BusesProperties().withInput("Input",juce::AudioChannelSet::stereo(),true).withOutput("Output",juce::AudioChannelSet::stereo(),true)), parameters(*this,nullptr,"STATE",makeLayout()) {}
void OptoValveProcessor::prepareToPlay(double sr, int maximumExpectedSamplesPerBlock)
{
    sampleRateHz=sr; detector={}; transformerFlux={0,0}; transformerBass={0,0}; transformerTreble={0,0}; outputCoreFlux={0,0}; outputCoreBass={0,0}; outputCoreTreble={0,0}; tubeBiasMemory={0,0}; tubeCathodeMemory={0,0}; fetOutputBias={0,0}; fetOutputHigh={0,0}; vintageHighCutState={0,0}; vintageHighCutBlend=0.0f; detectorBass={0,0}; sidechainEmphasisLow={0,0}; bassEnvelope=presenceEnvelope=0.0f; envelopeDb=slowMemory=smoothedGR=0; t4FastCell=t4SlowCell=t4Exposure=t4GainReduction=responseBlend=0.0f; gainReductionDb.store(0);
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
    const float scEmphasis=*parameters.getRawParameterValue("scEmphasis")*0.01f;
    // Detector-only high-frequency shelf: 0% neutral, 100% ~+9 dB at HF.
    const float scShelfBoost=std::pow(10.0f,9.0f*scEmphasis/20.0f)-1.0f;
    const float scShelfPole=std::exp(-2.0f*juce::MathConstants<float>::pi*1000.0f/(float)sampleRateHz);
    const float tube=*parameters.getRawParameterValue("tube")*0.01f;
    const float xfmr=*parameters.getRawParameterValue("transformer")*0.01f;
    const float mix=*parameters.getRawParameterValue("mix")*0.01f;
    const float vintageTarget=*parameters.getRawParameterValue("lofi")>0.5f ? 1.0f : 0.0f;
    // Gentle one-pole 12 kHz bandwidth limitation on the wet signal.
    const float vintagePole=std::exp(-2.0f*juce::MathConstants<float>::pi*12000.0f/(float)sampleRateHz);
    const float switchPole=coeff(0.015f,sampleRateHz);
    const int timingMode=(int)*parameters.getRawParameterValue("releaseStyle");
    const bool fastMode=timingMode==1;
    const bool smack=timingMode==2;
    const bool fetOutput=*parameters.getRawParameterValue("outputStage")>0.5f;
    const float responseTarget=*parameters.getRawParameterValue("response")>0.5f?1.0f:0.0f;
    const float responseSwitch=coeff(0.025f,sampleRateHz);
    // Published T4 response: about 10 ms attack; first half of recovery
    // around 40-80 ms, remainder potentially seconds and dependent on exposure.
    const float t4Attack=coeff(smack ? 0.0008f : 0.010f,sampleRateHz);
    const float t4FastDecay=coeff(smack ? 0.050f : 0.028f,sampleRateHz);
    const float t4ExposureAttack=coeff(smack ? 0.15f : 0.70f,sampleRateHz);
    const float t4ExposureRelease=coeff(smack ? 0.30f : 2.0f,sampleRateHz);
    const float hpA=1.0f/(1.0f+2.0f*juce::MathConstants<float>::pi*cutoff/(float)sampleRateHz);
    // Optical response: presence transients engage faster, bass energy builds more
    // gradually. Both feed ONE gain cell, as on an LA-2A (not a multiband compressor).
    const float bassSplit=std::exp(-2.0f*juce::MathConstants<float>::pi*320.0f/(float)sampleRateHz);
    const float bassAttack=coeff(smack ? 0.0008f : 0.035f,sampleRateHz);
    const float presenceAttack=coeff(smack ? 0.0008f : 0.010f,sampleRateHz);
    const float bassRelease=coeff(smack ? 0.050f : (fastMode ? 0.22f : 0.55f),sampleRateHz);
    const float presenceRelease=coeff(smack ? 0.050f : (fastMode ? 0.08f : 0.16f),sampleRateHz);
    const float fastRelease=coeff(smack ? 0.050f : (fastMode ? 0.09f : 0.16f),sampleRateHz);
    const float slowRelease=coeff(smack ? 0.050f : (fastMode ? 0.55f : 1.45f),sampleRateHz);
    const float memoryRelease=coeff(smack ? 0.050f : (fastMode ? 0.85f : 2.5f),sampleRateHz);
    const float grSmooth=coeff(0.003f,sampleRateHz);
    const float slope=1.0f-1.0f/ratio;
    // Reduction maps to an approximate optical detector threshold; no fixed 1:1 hardware calibration.
    const float threshold=-reduction;
    // Approximate transformer core behaviour: LF flux accumulates, its
    // saturation compresses the bass disproportionately, while HF passes
    // mostly linearly. This is a perceptual model, not a SPICE transformer.
    const float bassPole=std::exp(-2.0f*juce::MathConstants<float>::pi*180.0f/(float)sampleRateHz);
    const float fluxPole=std::exp(-2.0f*juce::MathConstants<float>::pi*65.0f/(float)sampleRateHz);
    const float treblePole=std::exp(-2.0f*juce::MathConstants<float>::pi*6500.0f/(float)sampleRateHz);
    const float coreDrive=1.0f+2.5f*xfmr;
    // The dry delay compensates for the oversampler's host-rate group delay.
    // Store the trimmed signal before gain reduction is applied.
    if(dryBuffer.getNumSamples()<n) dryBuffer.setSize(2,n,false,false,true);
    for(int i=0;i<n;++i)
    {
        float bassLevel=0.0f, presenceLevel=0.0f, classicLevel=0.0f;
        for(int c=0;c<channels;++c) {
            const float original=buffer.getReadPointer(c)[i]*inputGain;
            auto& bass=transformerBass[(size_t)c];
            auto& flux=transformerFlux[(size_t)c];
            auto& treble=transformerTreble[(size_t)c];
            bass=bassPole*bass+(1.0f-bassPole)*original;
            flux=fluxPole*flux+(1.0f-fluxPole)*bass;
            // Symmetric core limiting with a dynamic LF-dependent knee.
            // Normalising the derivative at zero preserves quiet detail.
            const float effectiveDrive=coreDrive*(1.0f+0.9f*xfmr*juce::jmin(2.0f,std::abs(flux)));
            const float saturatedBass=std::tanh(effectiveDrive*bass)/effectiveDrive;
            // Retain the mid/high band largely linear: avoids broadband tanh fizz.
            const float magnetic=original+xfmr*0.9f*(saturatedBass-bass);
            // Gentle level-dependent HF softening, not hard HF clipping.
            treble=treblePole*treble+(1.0f-treblePole)*magnetic;
            const float hfDamping=0.12f*xfmr*juce::jlimit(0.0f,1.0f,std::abs(flux)*1.5f);
            const float transformed=xfmr>0.0001f ? magnetic-hfDamping*(magnetic-treble) : original;
            buffer.getWritePointer(c)[i]=transformed;
            const float detected=detector[(size_t)c].process(transformed,hpA);
            auto& scLow=sidechainEmphasisLow[(size_t)c];
            scLow=scShelfPole*scLow+(1.0f-scShelfPole)*detected;
            const float weightedDetected=detected+scShelfBoost*(detected-scLow);
            classicLevel=juce::jmax(classicLevel,std::abs(weightedDetected));
            auto& low=detectorBass[(size_t)c];
            low=bassSplit*low+(1.0f-bassSplit)*weightedDetected;
            bassLevel=juce::jmax(bassLevel,std::abs(low));
            presenceLevel=juce::jmax(presenceLevel,std::abs(weightedDetected-low));
        }
        const float bassCoef=bassLevel>bassEnvelope?bassAttack:bassRelease;
        const float presenceCoef=presenceLevel>presenceEnvelope?presenceAttack:presenceRelease;
        bassEnvelope=bassCoef*bassEnvelope+(1.0f-bassCoef)*bassLevel;
        presenceEnvelope=presenceCoef*presenceEnvelope+(1.0f-presenceCoef)*presenceLevel;
        // Reduced LF detector weight mitigates bass-driven pumping; presence still
        // catches pick attacks. The gain change remains broadband.
        const float weightedLevel=std::sqrt(0.36f*bassEnvelope*bassEnvelope+
                                            presenceEnvelope*presenceEnvelope);
        const float levelDb=juce::Decibels::gainToDecibels(juce::jmax(weightedLevel,1.0e-8f),-160.0f);
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
        const float envCoef=target>envelopeDb?presenceAttack:release;
        envelopeDb=envCoef*envelopeDb+(1.0f-envCoef)*target;
        smoothedGR=grSmooth*smoothedGR+(1.0f-grSmooth)*envelopeDb;
        // Classic T4 uses the same optical gain cell but its own broadband
        // detector and two-component photocell recovery with illumination memory.
        const float classicDb=juce::Decibels::gainToDecibels(juce::jmax(classicLevel,1.0e-8f),-160.0f);
        const float classicAbove=classicDb-threshold;
        float classicTarget=0.0f;
        if(knee<0.01f) classicTarget=juce::jmax(0.0f,classicAbove)*slope;
        else if(classicAbove>=knee*0.5f) classicTarget=classicAbove*slope;
        else if(classicAbove>-knee*0.5f) {
            const float t=classicAbove+knee*0.5f;
            classicTarget=slope*t*t/(2.0f*knee);
        }
        // Exposure integrates sustained and strong illumination, causing a
        // larger, longer-lasting slow tail after prolonged gain reduction.
        const float illumination=juce::jlimit(0.0f,1.0f,classicTarget/18.0f);
        const float exposureCoef=illumination>t4Exposure?t4ExposureAttack:t4ExposureRelease;
        t4Exposure=exposureCoef*t4Exposure+(1.0f-exposureCoef)*illumination;
        const float t4SlowTime=smack ? 0.050f : (1.0f+11.0f*t4Exposure);
        const float t4SlowDecay=coeff(t4SlowTime,sampleRateHz);
        t4FastCell=(classicTarget>t4FastCell?t4Attack:t4FastDecay)*t4FastCell
                   +(1.0f-(classicTarget>t4FastCell?t4Attack:t4FastDecay))*classicTarget;
        t4SlowCell=(classicTarget>t4SlowCell?t4Attack:t4SlowDecay)*t4SlowCell
                   +(1.0f-(classicTarget>t4SlowCell?t4Attack:t4SlowDecay))*classicTarget;
        const float slowFraction=0.10f+0.42f*t4Exposure;
        const float classicGR=(1.0f-slowFraction)*t4FastCell+slowFraction*t4SlowCell;
        // Smooth response-mode changes in the gain domain to prevent clicks.
        responseBlend=responseSwitch*responseBlend+(1.0f-responseSwitch)*responseTarget;
        t4GainReduction=classicGR;
        const float gain=dbToGain(-(smoothedGR+(t4GainReduction-smoothedGR)*responseBlend));
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
    // Tube nonlinearities are evaluated at 4x the host sample rate to reduce aliasing.
    // The transformer core proxy above currently runs at the host rate.
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
            for(int j=0;j<upCount;++j)
            {
                float wet=samples[j];
                if(!fetOutput && tube>0.0001f)
                {
                    // Valve gain-stage proxy: asymmetric grid conduction plus
                    // cathode self-bias recovery, calculated at 4x rate.
                    const float drive=1.0f+1.5f*tube;
                    auto& biasState=tubeBiasMemory[(size_t)c];
                    auto& cathode=tubeCathodeMemory[(size_t)c];
                    const float cathodePole=std::exp(-2.0f*juce::MathConstants<float>::pi*35.0f/(float)(sampleRateHz*4.0));
                    cathode=cathodePole*cathode+(1.0f-cathodePole)*std::abs(wet);
                    const float biasPole=std::exp(-2.0f*juce::MathConstants<float>::pi*9.0f/(float)(sampleRateHz*4.0));
                    biasState=biasPole*biasState+(1.0f-biasPole)*juce::jmax(0.0f,wet-0.4f);
                    const float bias=0.08f*tube+0.13f*tube*cathode-0.10f*tube*biasState;
                    const float z=wet*drive+bias;
                    const float asymmetric=std::tanh(z)-0.035f*tube*std::tanh(z*z);
                    const float baseline=std::tanh(bias)-0.035f*tube*std::tanh(bias*bias);
                    const float smallSignal=1.0f-std::tanh(bias)*std::tanh(bias);
                    wet=(asymmetric-baseline)/(drive*juce::jmax(0.15f,smallSignal));
                }
                if(fetOutput) {
                    // 1176-style Class A output amplifier AFTER the optical gain cell,
                    // BEFORE the output transformer. Tube Drive controls amplifier colour.
                    const float rate=(float)(sampleRateHz*4.0);
                    auto& biasState=fetOutputBias[(size_t)c];
                    auto& highState=fetOutputHigh[(size_t)c];
                    const float bp=std::exp(-2.0f*juce::MathConstants<float>::pi*24.0f/rate);
                    const float hp=std::exp(-2.0f*juce::MathConstants<float>::pi*8500.0f/rate);
                    biasState=bp*biasState+(1.0f-bp)*std::abs(wet);
                    const float drive=1.0f+0.65f*tube;
                    const float bias=0.055f*tube*biasState;
                    const float z=wet*drive+bias;
                    const float shaped=(std::tanh(z)-std::tanh(bias))/
                        (drive*juce::jmax(0.2f,1.0f-std::tanh(bias)*std::tanh(bias)));
                    highState=hp*highState+(1.0f-hp)*shaped;
                    wet=shaped-0.05f*tube*(shaped-highState);
                }
                // Separate LA-2A-inspired output transformer. LF magnetisation
                // has a slower memory than the input stage; nonlinear loss
                // rises with flux and level. This is not a measured core model.
                auto& outBass=outputCoreBass[(size_t)c];
                auto& outFlux=outputCoreFlux[(size_t)c];
                auto& outTreble=outputCoreTreble[(size_t)c];
                const float osRate=(float)(sampleRateHz*4.0);
                const float lowPole=std::exp(-2.0f*juce::MathConstants<float>::pi*(fetOutput?155.0f:125.0f)/osRate);
                const float fluxPole=std::exp(-2.0f*juce::MathConstants<float>::pi*(fetOutput?43.0f:28.0f)/osRate);
                const float hfPole=std::exp(-2.0f*juce::MathConstants<float>::pi*(fetOutput?12500.0f:11500.0f)/osRate);
                outBass=lowPole*outBass+(1.0f-lowPole)*wet;
                outFlux=fluxPole*outFlux+(1.0f-fluxPole)*outBass;
                const float outputDrive=fetOutput ? 1.55f*(1.0f+0.35f*juce::jmin(2.0f,std::abs(outFlux))) : 1.0f+1.8f*xfmr*(1.0f+0.3f*juce::jmin(2.0f,std::abs(outFlux)));
                const float saturated=std::tanh(outputDrive*outBass)/outputDrive;
                wet+=(fetOutput ? 0.12f*xfmr : 0.40f*xfmr)*(saturated-outBass);
                outTreble=hfPole*outTreble+(1.0f-hfPole)*wet;
                wet-=(fetOutput ? 0.02f : 0.035f)*xfmr*juce::jmin(1.0f,std::abs(outFlux))*(wet-outTreble);
                samples[j]=wet;
            }
        }
        colourOversampling->processSamplesDown(inputSlice);
        // Smooth the switch once per sample, shared across channels.
        for(int j=0;j<count;++j)
        {
            vintageHighCutBlend=switchPole*vintageHighCutBlend+(1.0f-switchPole)*vintageTarget;
            for(int c=0;c<channels;++c)
            {
                float* dest=buffer.getWritePointer(c,offset);
                const float* wet=colourBuffer.getReadPointer(c);
                auto& filtered=vintageHighCutState[(size_t)c];
                filtered=vintagePole*filtered+(1.0f-vintagePole)*wet[j];
                const float vintageWet=wet[j]+vintageHighCutBlend*(filtered-wet[j]);
                // Apply the vintage bandwidth only to the processed wet path.
                dest[j]=vintageWet*makeup*mix + dryBuffer.getReadPointer(c,offset)[j]*(1.0f-mix);
            }
        }
    }
    gainReductionDb.store(juce::jmax(0.0f,smoothedGR+(t4GainReduction-smoothedGR)*responseBlend),std::memory_order_relaxed);
}
juce::AudioProcessorEditor* OptoValveProcessor::createEditor() { return new OptoValveEditor(*this); }
void OptoValveProcessor::getStateInformation(juce::MemoryBlock& b) { auto xml=parameters.copyState().createXml(); copyXmlToBinary(*xml,b); }
void OptoValveProcessor::setStateInformation(const void* data,int bytes) { if(auto xml=getXmlFromBinary(data,bytes)) if(xml->hasTagName(parameters.state.getType())) parameters.replaceState(juce::ValueTree::fromXml(*xml)); }
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new OptoValveProcessor(); }
