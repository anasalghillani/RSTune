#pragma once
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

// Minimal 32-bit float mono WAV writer, enough to audition the harness output.
inline bool WriteWavMono32f(const std::string& path, const std::vector<float>& data, int sampleRate)
{
	FILE* f = nullptr;
	if (fopen_s(&f, path.c_str(), "wb") != 0 || !f)
		return false;

	const uint32_t dataBytes = (uint32_t)(data.size() * sizeof(float));
	const uint32_t fmtSize = 16;
	const uint16_t fmtTag = 3;      // IEEE float
	const uint16_t channels = 1;
	const uint16_t bits = 32;
	const uint32_t byteRate = (uint32_t)sampleRate * channels * (bits / 8);
	const uint16_t blockAlign = channels * (bits / 8);
	const uint32_t riffSize = 36 + dataBytes;

	fwrite("RIFF", 1, 4, f); fwrite(&riffSize, 4, 1, f);
	fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f); fwrite(&fmtSize, 4, 1, f);
	fwrite(&fmtTag, 2, 1, f); fwrite(&channels, 2, 1, f);
	fwrite(&sampleRate, 4, 1, f); fwrite(&byteRate, 4, 1, f);
	fwrite(&blockAlign, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
	fwrite(data.data(), 1, dataBytes, f);
	fclose(f);
	return true;
}
