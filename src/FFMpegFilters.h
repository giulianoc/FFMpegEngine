/*
 * To change this license header, choose License Headers in Project Properties.
 * To change this template file, choose Tools | Templates
 * and open the template in the editor.
 */

/*
 * File:   FFMPEGFilters.h
 *
 * Created on February 18, 2018, 1:27 AM
 */

#pragma once

#include "nlohmann/json.hpp"
#include "spdlog/spdlog.h"
// #include <chrono>
#include <string>

class FFMpegFilters
{
  public:
	FFMpegFilters(std::string ffmpegTempDir, std::string ffmpegTtfFontDir, int64_t ingestionJobKey, int64_t encodingJobKey, int outputIndex = -1);

	~FFMpegFilters();

	std::tuple<std::string, std::string, std::string>
	addFilters(
		nlohmann::json filtersRoot, const std::string& ffmpegVideoResolutionParameter, const std::string& ffmpegDrawTextFilter,
		std::optional<int32_t> inputDurationInSeconds) const;

	[[nodiscard]] std::string addVideoFilters(
		nlohmann::json filtersRoot, const std::string &ffmpegVideoResolutionParameter, const std::string &ffmpegDrawTextFilter,
		std::optional<int32_t> inputDurationInSeconds
	) const;

	[[nodiscard]] std::string addAudioFilters(const nlohmann::json &filtersRoot, std::optional<int32_t> inputDurationInSeconds) const;

	[[nodiscard]] std::string getFilter(const nlohmann::json& filterRoot, std::optional<int32_t> inputDurationInSeconds) const;

	static nlohmann::json mergeFilters(const nlohmann::json &filters_1Root, const nlohmann::json &filters_2Root);

	static std::string getDrawTextTemporaryPathName(const std::string &ffmpegTempDir, int64_t ingestionJobKey, int64_t encodingJobKey, int outputIndex);
	static nlohmann::json createTimecodeDrawTextFilter();

  private:
	std::string _ffmpegTempDir;
	std::string _ffmpegTtfFontDir;

	int64_t _ingestionJobKey;
	int64_t _encodingJobKey;
	int _outputIndex;
};
