
#pragma once

#include <SDL3/SDL.h>

#include <string>
#include <print>


struct WaveStream
{
    std::string Name = "";
    SDL_AudioStream* Stream = nullptr;
    uint8_t* WaveData = nullptr;
    uint32_t WaveSize = 0;

    SDL_AudioSpec ImportSpec;
    SDL_AudioSpec TargetSpec;

    WaveStream()
    {
    }

    WaveStream(const SDL_AudioSpec& InTargetSpec, const char* Path, bool UseImportFrequency = false)
        : TargetSpec(InTargetSpec)
    {
        Name = Path;
        const std::string FullPath = std::format("{}{}", SDL_GetBasePath(), Path);

        if (SDL_LoadWAV(FullPath.c_str(), &ImportSpec, &WaveData, &WaveSize))
        {
            if (UseImportFrequency)
            {
                TargetSpec.freq = ImportSpec.freq;
            }

            std::print("Opening {}\n", FullPath);
            std::print(" - Frequency: {} -> {}\n", ImportSpec.freq, TargetSpec.freq);
            std::print(" - Channels: {} -> {}\n", ImportSpec.channels, TargetSpec.channels);
            std::print(" - Float: {} -> {}\n",
                bool(SDL_AUDIO_ISFLOAT(ImportSpec.format)), bool(SDL_AUDIO_ISFLOAT(TargetSpec.format)));

            std::print(" - Word Size: {} -> {}\n",
                SDL_AUDIO_BYTESIZE(ImportSpec.format), SDL_AUDIO_BYTESIZE(TargetSpec.format));
            std::print("\n");

            Stream = SDL_CreateAudioStream(&ImportSpec, &TargetSpec);
            SDL_PutAudioStreamData(Stream, WaveData, WaveSize);
            SDL_FlushAudioStream(Stream);
        }
        else
        {
            std::print("Couldn't load {}: {}\n", FullPath, SDL_GetError());
            Reset("error handler");
        }
    }

    void Transcode(std::vector<float>& OutSamples)
    {
        if (Stream)
        {
            uint32_t ImportFrameSize = SDL_AUDIO_FRAMESIZE(ImportSpec);
            uint32_t TargetFrameSize = SDL_AUDIO_FRAMESIZE(TargetSpec);

            uint32_t SampleCount = WaveSize / ImportFrameSize;
            OutSamples.resize(SampleCount);

            uint32_t OutBytes = SampleCount * TargetFrameSize;
            uint8_t* TargetData = (uint8_t*)OutSamples.data();
            SDL_GetAudioStreamData(Stream, TargetData, OutBytes);
            Reset("transcoder");
        }
        else
        {
            OutSamples.clear();
        }
    }


    void Reset(const char* Hint)
    {
        if (Stream)
        {
            SDL_DestroyAudioStream(Stream);
            Stream = nullptr;
            std::print("Stream \"{}\" closed by {}.\n", Name, Hint);
        }
        if (WaveData)
        {
            SDL_free(WaveData);
            WaveData = nullptr;
            WaveSize = 0;
        }
    }


    ~WaveStream()
    {
        Reset("destructor");
    }
};


struct WaveData
{
    std::vector<float> Samples;

    WaveData()
    {
    }


    WaveData(SDL_AudioSpec& TargetSpec, const char* Path, bool UseImportFrequency = false)
    {
        WaveStream Stream(TargetSpec, Path, UseImportFrequency);
        Stream.Transcode(Samples);

        if (UseImportFrequency)
        {
            TargetSpec.freq = Stream.ImportSpec.freq;
        }
    }

    void NormalizeImpulseResponse()
    {
        float Acc = 0.0f;
        for (const float& Sample : Samples)
        {
            Acc += std::abs(Sample);
        }

        const float IdealLevel = 0.5;
        const float Scale = IdealLevel / std::sqrt(Acc); // entirely intuition, but holds up so far under experimentation

        for (float& Sample : Samples)
        {
            Sample *= Scale;
        }
    }
};
