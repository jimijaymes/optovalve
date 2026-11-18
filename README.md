# OptoValve — LA-2A-inspired VST3 compressor

An original, 64-bit VST3 audio plugin project using JUCE 8 and CMake. This is **not** an official LA-2A model, a circuit-accurate emulation, or a Universal Audio product.

## Features
- Linked stereo optical-style compression with nominal 4:1 / 10:1 settings
- Adjustable 0–24 dB knee
- 20–500 Hz sidechain detector high-pass filter (internal sidechain only)
- Programme-dependent two-stage release (fast initial recovery, slow memory tail)
- Adjustable asymmetric tube-like saturation and transformer-like saturation
- Peak Reduction, makeup gain, wet/dry mix
- Host parameter automation and preset/state recall

## Build (Windows 64-bit)
1. Install Visual Studio 2022 with **Desktop development with C++**, CMake, and Git.
2. Open a terminal in this folder and run:
   ```powershell
   cmake -S . -B build -A x64
   cmake --build build --config Release --target OptoValve_VST3
   ```
3. JUCE is fetched from GitHub on first configure (internet needed). Locate `OptoValve.vst3` under `build/OptoValve_artefacts/Release/VST3/`.
4. Copy the `.vst3` bundle into `C:\Program Files\Common Files\VST3\` and rescan plugins in your DAW.

## Engineering notes
This is a functional first-pass algorithm, not a measurement-matched hardware model. Ratios are nominal static curve slopes, not verified LA-2A behaviour. The attack/release are simplified optical-style envelopes; true T4 electro-luminescent/photoresistor memory is considerably more complex. Tube and transformer sections use efficient waveshapers rather than physical circuit models. Saturation is now **4x oversampled** to reduce aliasing. The custom interface displays gain reduction. No external sidechain input. Audio output has not been validated in Cubase or through a hardware reference comparison.

## Custom interface (v1.1)
The project now includes a custom, vintage rack-inspired JUCE editor with seven rotary knobs, operating-mode selection and a live gain-reduction needle meter. All controls use JUCE APVTS attachments for automation and session recall. The interface is an original design, not an exact Teletronix panel reproduction.

## Build status
This archive is **source code only**. The current environment has no JUCE SDK, cannot reach GitHub to fetch it, and does not have the Windows MSVC toolchain. It has therefore **not been compiled or host-tested**. On Windows with Visual Studio 2022 and internet access, use:

```
cmake -S . -B build -A x64
cmake --build build --config Release --target OptoValve_VST3
```

If JUCE CMake fails due to a specific JUCE version/API change, adjust against your installed JUCE version. Test the resulting VST3 in a plugin validator before using it in production.

## GitHub Actions: automated Windows builds

1. Create a new GitHub repository (private is fine).
2. Upload the **contents of this OptoValve folder** to the repository root, including the hidden `.github/workflows/windows-vst3.yml` file. GitHub's **Add file → Upload files** may not preserve hidden folders reliably; GitHub Desktop or Git CLI is recommended.
3. Push to `main` or open the repository **Actions → Build Windows VST3 → Run workflow**.
4. After the run succeeds, open its summary and download the `OptoValve-Windows-x64-VST3` artifact. Extract the ZIP inside it, then copy the `OptoValve.vst3` directory to `C:\Program Files\Common Files\VST3\`.
5. Rescan in Cubase. If the workflow fails, copy the red build-log error lines for diagnosis.

### 4x oversampling
The tube and transformer nonlinearities are now processed at **4x the host rate** using JUCE's polyphase IIR oversampler, with oversampling latency reported to the host and a delayed dry path for parallel mixing. This reduces, but does not eliminate, aliasing. The compressor detector and envelope run at host sample rate. Oversampling is fixed at 4x to avoid latency changes during playback. This code has not been compiled or host-tested in this environment.

### Known engineering caveats
- The dry scratch buffer can resize if a host delivers a block larger than its declared maximum; avoid doing so in a hard real-time host.
- Saturation is simplified rather than circuit-measured. Check phase, latency, bypass behaviour, automation, mono/stereo, and distortion spectra before production use.
- The workflow produces an unsigned Windows plugin. Windows may show a publisher warning; it is not an installer.
