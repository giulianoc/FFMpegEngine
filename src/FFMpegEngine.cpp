
#include "FFMpegEngine.h"

#include "Datetime.h"
#include "StringUtils.h"
#include "spdlog/spdlog.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <ranges>
#include <sstream>

// ---------------- Input methods ----------------

FFMpegEngine::Input& FFMpegEngine::Input::addArg(const  std::string_view& parameter)
{
	 std::string_view trimmed = StringUtils::trim(parameter);
	if (!trimmed.empty())
		_args.emplace_back(trimmed);
	return *this;
}

FFMpegEngine::Input& FFMpegEngine::Input::addArgs(const  std::string& parameters)
{
	for (auto&& tok :
		parameters | std::views::split(' ') | std::views::filter([](auto &&rng){ return !std::ranges::empty(rng); }))
		_args.emplace_back(tok.begin(), tok.end());
	return *this;
}

void FFMpegEngine::Input::buildArgs( std::vector< std::string>& args) const
{
	for (auto& arg : _args)
		args.emplace_back(arg);
	if (_durationSeconds > 0)
	{
		args.emplace_back("-t");
		args.emplace_back(std::format("{}", _durationSeconds));
	}
	args.emplace_back("-i");
	args.emplace_back(_source);
}

 std::string FFMpegEngine::Input::toSingleLine() const
{
	 std::vector< std::string> args;
	buildArgs(args);

	return FFMpegEngine::toSingleLine(args);
}


// ---------------- Output methods ----------------

FFMpegEngine::Output& FFMpegEngine::Output::addArg(const  std::string_view& parameter)
{
	 std::string_view trimmed = StringUtils::trim(parameter);
	if (!trimmed.empty())
		_extraArgs.emplace_back(trimmed);
	return *this;
}

FFMpegEngine::Output& FFMpegEngine::Output::addArgs(const  std::string& parameters)
{
	for (auto&& tok :
		parameters | std::views::split(' ') | std::views::filter([](auto &&rng){ return !std::ranges::empty(rng); }))
		_extraArgs.emplace_back(tok.begin(), tok.end());
	return *this;
}

void FFMpegEngine::Output::buildArgs( std::vector< std::string>& args) const
{
	for (auto& map : _maps)
	{
		args.emplace_back("-map");
		args.emplace_back(map);
	}
	if (_copyAllTracks)
	{
		args.emplace_back("-c");
		args.emplace_back("copy");
	}
	else
	{
		if (_videoCodec)
		{
			args.emplace_back("-c:v");
			args.emplace_back(*_videoCodec);
		}
		if (_audioCodec)
		{
			args.emplace_back("-c:a");
			args.emplace_back(*_audioCodec);
		}
	}
	if (!_videoFilters.empty())
	{
		 std::string vf;
		for (size_t index=0; index < _videoFilters.size(); ++index)
		{
			vf += _videoFilters[index];
			if (index + 1 < _videoFilters.size())
				vf += ",";
		}
		args.emplace_back("-vf");
		args.emplace_back(vf);
	}
	if (!_audioFilters.empty())
	{
		 std::string af;
		for (size_t index = 0; index < _audioFilters.size(); ++index)
		{
			af += _audioFilters[index];
			if (index + 1 < _audioFilters.size())
				af += ",";
		}
		args.emplace_back("-af");
		args.emplace_back(af);
	}
	for (auto& extraArg : _extraArgs)
		args.emplace_back(extraArg);
	if (!_path.empty())
		args.emplace_back(_path);
}

void FFMpegEngine::run(const  std::string& ffmpegPath, ProcessUtility::ProcessId& processId,
	int &iReturnedStatus, const  std::string& referenceToLog,
	const std::shared_ptr<CallbackData> &clientCallbackData, const  std::string& outputFfmpegPathFileName)
{
	try
	{
		if (clientCallbackData)
		{
			LOG_INFO("run (client callback)"
				", ffmpegPath: {}"
				"{}"
				", outputFfmpegPathFileName: {}"
				", durationMilliSeconds: {}"
				", args: {}",
				ffmpegPath, referenceToLog, outputFfmpegPathFileName,
				_durationMilliSeconds ? *_durationMilliSeconds : static_cast<int64_t>(-1),
				toSingleLine(true)
				);
			_clientCallbackData = clientCallbackData;
			if (!outputFfmpegPathFileName.empty())
				_clientCallbackData->setOutputFfmpegPathFileName(outputFfmpegPathFileName);
		}
		else
		{
			LOG_INFO("run (internal callback)"
				", ffmpegPath: {}"
				"{}"
				", outputFfmpegPathFileName: {}"
				", durationMilliSeconds: {}"
				", args: {}",
				ffmpegPath, referenceToLog, outputFfmpegPathFileName,
				_durationMilliSeconds ? *_durationMilliSeconds : static_cast<int64_t>(-1),
				toSingleLine(true)
				);
			_clientCallbackData = nullptr;
			_internalCallbackData->reset();
			_internalCallbackData->setOutputFfmpegPathFileName(outputFfmpegPathFileName);
		}
		_referenceToLog = referenceToLog;
		ProcessUtility::forkAndExecByCallback(
			std::format("{}/ffmpeg", ffmpegPath), buildArgs(true),
			[&](const  std::string_view& line) {ffmpegLineCallback(line); },
			true, true, processId, iReturnedStatus);
	}
	catch (std::exception& e)
	{
		LOG_ERROR("run failed"
			"{}"
			", exception: {}", referenceToLog, e.what()
			);
		throw;
	}
}

void FFMpegEngine::ffmpegLineCallback(const  std::string_view& ffmpegLine)
{
	try
	{
		const std::shared_ptr<CallbackData> callbackData = _clientCallbackData ? _clientCallbackData : _internalCallbackData;

		// la prima chiamata ricevuta setta finished a false
		if (!callbackData->_finished)
		{
			callbackData->_finished = false;
			callbackData->_startTime = std::chrono::system_clock::now();
		}
		else if (ffmpegLine.empty()) // ffmpegLine vuoto indica fine scrittura su file
			callbackData->_endTime = std::chrono::system_clock::now();

		// su questo file di log scrivo gli errori e tutto cio che non è gestito
		if (!callbackData->_outputFfmpegPathFileName.empty())
		{
			if (!ffmpegLine.empty())
			{
				if (!callbackData->_ffmpegOutputLogFile)
				{
					callbackData->_ffmpegOutputLogFile.open(callbackData->_outputFfmpegPathFileName,
						std::ofstream::binary | std::ofstream::trunc);
					if (!callbackData->_ffmpegOutputLogFile)
					{
						 std::string errorMessage = std::format(
							"ffmpegLineCallback, open file failed"
							"{}"
							", ffmpegOutputLogPathFileName: {}",
							_referenceToLog, callbackData->_outputFfmpegPathFileName
						);
						LOG_ERROR(errorMessage);
						callbackData->pushErrorMessage(std::format("{} {}",
							Datetime::nowLocalTime("%Y-%m-%d %H:%M:%S.", true), errorMessage));
					}
				}
			}
			else
			{
				// ffmpegLine vuoto indica fine scrittura su file
				if (callbackData->_ffmpegOutputLogFile)
					callbackData->_ffmpegOutputLogFile.close();
			}
		}

		// detect errors
		bool error = false;
		{
			const  std::string ffmpegLineLower = StringUtils::lowerCase(ffmpegLine);

			// known errors
			for (auto &pattern : FFMpegEngine::CallbackData::errorPatterns)
			{
				if (ffmpegLineLower.find(pattern) != std::string::npos)
				{
					callbackData->pushErrorMessage(std::format("{} {}: {}",
						Datetime::nowLocalTime("%Y-%m-%d %H:%M:%S.", true), pattern, ffmpegLine));
					LOG_ERROR("ffmpegLineCallback, {} detected"
						"{}"
						", ffmpegLine: {}", pattern, _referenceToLog, ffmpegLine);
					error = true;
					if (callbackData->_ffmpegOutputLogFile)
					{
						const  std::string dateInfo = std::format("[{}] ",
							Datetime::nowLocalTime("%Y-%m-%d %H:%M:%S.", true));
						callbackData->_ffmpegOutputLogFile.write(dateInfo.data(), dateInfo.size());
						callbackData->_ffmpegOutputLogFile.write(ffmpegLine.data(), ffmpegLine.size());
						callbackData->_ffmpegOutputLogFile.write("\n", 1);
						callbackData->_ffmpegOutputLogFile.flush();
					}
				}
			}
			if (!error)
			{
				// generic error
				if (ffmpegLineLower.find("error") != std::string::npos)
				{
					callbackData->pushErrorMessage(std::format("{} error: {}",
						Datetime::nowLocalTime("%Y-%m-%d %H:%M:%S.", true), ffmpegLine));
					LOG_ERROR("ffmpegLineCallback, error detected"
						"{}"
						", ffmpegLine: {}", _referenceToLog, ffmpegLine);
					error = true;
					if (callbackData->_ffmpegOutputLogFile)
					{
						const  std::string dateInfo = std::format("[{}] ",
							Datetime::nowLocalTime("%Y-%m-%d %H:%M:%S.", true));
						callbackData->_ffmpegOutputLogFile.write(dateInfo.data(), dateInfo.size());
						callbackData->_ffmpegOutputLogFile.write(ffmpegLine.data(), ffmpegLine.size());
						callbackData->_ffmpegOutputLogFile.write("\n", 1);
						callbackData->_ffmpegOutputLogFile.flush();
					}
				}
				else if (ffmpegLineLower.find("signal") != std::string::npos
					&& ffmpegLineLower.find("timefromsignal") == std::string::npos)
				{
					if (ffmpegLineLower.find("signal 3") !=  std::string::npos // SIGQUIT
						|| ffmpegLineLower.find("signal: 3") !=  std::string::npos)
						callbackData->_signal = 3;
					else if (ffmpegLineLower.find("signal 15") !=  std::string::npos // SIGTERM
						|| ffmpegLineLower.find("signal: 15") !=  std::string::npos)
						callbackData->_signal = 15;

					callbackData->pushErrorMessage(std::format("{} signal: {}",
						Datetime::nowLocalTime("%Y-%m-%d %H:%M:%S.", true), ffmpegLine));
					LOG_ERROR("ffmpegLineCallback, signal detected"
						"{}"
						", ffmpegLine: {}", _referenceToLog, ffmpegLine);
					error = true;
					if (callbackData->_ffmpegOutputLogFile)
					{
						const  std::string dateInfo = std::format("[{}] ",
							Datetime::nowLocalTime("%Y-%m-%d %H:%M:%S.", true));
						callbackData->_ffmpegOutputLogFile.write(dateInfo.data(), dateInfo.size());
						callbackData->_ffmpegOutputLogFile.write(ffmpegLine.data(), ffmpegLine.size());
						callbackData->_ffmpegOutputLogFile.write("\n", 1);
						callbackData->_ffmpegOutputLogFile.flush();
					}
				}
			}
		}

		if (!error)
		{
			auto pos = ffmpegLine.find('=');
			if (pos !=  std::string_view::npos)
			{
				 std::string_view key = StringUtils::trim(ffmpegLine.substr(0, pos));
				 std::string_view value = StringUtils::trim(ffmpegLine.substr(pos + 1));
				if (value.find('=') !=  std::string_view::npos)
				{
					// Questo if per gestire casi come:
					// frame=11 11 fps= 11 q=28.0 q=28.0 size=N/A time=00:00:00.36 bitrate=N/A dup=2 drop=0 speed=0.358x
					// In questo caso inizializziamo una key vuote in modo che vada nel default dello switch sotto
					key = ffmpegLine.substr(0, 0);
				}
				bool realBitRateChanged = false;
				switch (hash_case(key))
				{
					case "frame"_case:
					{
						callbackData->_processedFrames = stoi( std::string(value));
						break;
					}
					case "fps"_case:
					{
						callbackData->_framePerSeconds = stod( std::string(value));
						break;
					}
					case "speed"_case:
					{
						if (value != "N/A")
						{
							if (value.back() == 'x')
								value.remove_suffix(1);
							callbackData->_speed = std::stod( std::string(value));
						}
						break;
					}
					case "drop_frames"_case:
					{
						callbackData->_dropFrames = stoi( std::string(value));
						break;
					}
					case "dup_frames"_case:
					{
						callbackData->_dupFrames = stoi( std::string(value));
						break;
					}
					case "stream_0_0_q"_case:
					{
						callbackData->_stream_0_0_q = std::stod( std::string(value));
						break;
					}
					case "stream_1_0_q"_case:
					{
						callbackData->_stream_1_0_q = std::stod( std::string(value));
						break;
					}
					case "out_time"_case:
					{
						// usiamo out_time_ms already in millisecs
						/*
						// formato: HH:MM:SS.xxx
						int h = std::stoi( std::string(value.substr(0, 2)));
						int m = std::stoi( std::string(value.substr(3, 2)));
						int s = std::stoi( std::string(value.substr(6, 2)));
						int ms = std::stoi( std::string(value.substr(9)));
						_encoding->_progress.out_time = std::chrono::milliseconds(
							(static_cast<int64_t>(h) * 3600000LL) +
							(static_cast<int64_t>(m) * 60000LL) +
							(static_cast<int64_t>(s) * 1000LL) +
							ms);
						*/
						break;
					}
					case "out_time_us"_case: // timestamp dell'output in microsecondi (1.000.000), usiamo out_time_ms in millisecs
					{
						if (value != "N/A")
						{
							callbackData->_processedOutputTimestampMilliSecs = std::chrono::milliseconds(stoul( std::string(value)) / 1000);
							realBitRateChanged = true;
						}
						break;
					}
					case "out_time_ms"_case:
					{
						// Dovrebbe contenere millisecondi, invece contiene microsecondi, come out_time_us
						// È un bug storico mai sistemato per non rompere script esistenti. E' sempre identico a out_time_us
						// Per cui uso out_time_us

						/*
						// formato: HH:MM:SS.xxx
						int h = std::stoi( std::string(value.substr(0, 2)));
						int m = std::stoi( std::string(value.substr(3, 2)));
						int s = std::stoi( std::string(value.substr(6, 2)));
						int ms = std::stoi( std::string(value.substr(9)));
						_encoding->_progress.out_time = std::chrono::milliseconds(
							(static_cast<int64_t>(h) * 3600000LL) +
							(static_cast<int64_t>(m) * 60000LL) +
							(static_cast<int64_t>(s) * 1000LL) +
							ms);
						*/
						break;
					}
					case "total_size"_case:
					{
						if (value != "N/A")
						{
							callbackData->_processedSizeKBps = stoul( std::string(value));
							realBitRateChanged = true;
						}
						break;
					}
					case "bitrate"_case:
					{
						// value is in kbits/s
						if (value != "N/A")
						{
							callbackData->_bitRateKbps = stod(std::string(value));
							realBitRateChanged = true;
						}
						break;
					}
					case "progress"_case:
					{
						if (value == "end")
							callbackData->_finished = true;
						break;
					}
					default:
					{
						 std::string cleanffmpegLine;
						{
							cleanffmpegLine.reserve(ffmpegLine.size());
							for (char c : ffmpegLine) {
								if (c != '\r')   // elimina il carriage return
									cleanffmpegLine.push_back(c);
							}
						}

						LOG_WARN("ffmpegLineCallback, line not managed"
							"{}"
							", cleanffmpegLine (without \\r): {}", _referenceToLog, cleanffmpegLine);

						if (callbackData->_ffmpegOutputLogFile)
						{
							const  std::string dateInfo = std::format("[{}] ",
								Datetime::nowLocalTime("%Y-%m-%d %H:%M:%S.", true));
							callbackData->_ffmpegOutputLogFile.write(dateInfo.data(), dateInfo.size());
							callbackData->_ffmpegOutputLogFile.write(cleanffmpegLine.data(), cleanffmpegLine.size());
							callbackData->_ffmpegOutputLogFile.write("\n", 1);
							callbackData->_ffmpegOutputLogFile.flush();
						}
						break;
					}
				}

				// NEW: calcolo del bitrate reale
				if (realBitRateChanged && callbackData->_processedOutputTimestampMilliSecs.count() > 0 && callbackData->_processedSizeKBps > 0)
				{
					const double seconds = callbackData->_processedOutputTimestampMilliSecs.count() / 1000.0;
					const double realBps = (callbackData->_processedSizeKBps * 8.0) / seconds; // kilobytes -> kilobits
					const double realKbps = realBps * 1000.0;

					// Media ponderata per stabilità:
					if (callbackData->_bitRateKbps > 0.0)
						callbackData->_avgBitRateKbps = (callbackData->_bitRateKbps * 0.6) + (realKbps * 0.4);
					else
						callbackData->_avgBitRateKbps = realKbps;
				}

				if (callbackData->_processedOutputTimestampMilliSecs.count() > 0 && _durationMilliSeconds && *_durationMilliSeconds > 0)
				{
					double dValue = (static_cast<double>(callbackData->_processedOutputTimestampMilliSecs.count()) * 100.0) /
						static_cast<double>(*(_durationMilliSeconds));
					// Prossima istruzione serve per arrotondare, *100 sposta due cifre decimali a sinistra, round arrotonda, /100 riporta a posto
					callbackData->_progressPercent = std::round(dValue * 100.0) / 100.0;
					if (*callbackData->_progressPercent > 100.0)
						callbackData->_progressPercent = 100.0;
				}
				LOG_TRACE("ffmpegLineCallback, progressPercent"
					"{}"
					", processedOutputTimestampMilliSecs: {}"
					", durationMilliSeconds: {}"
					", progressPercent: {}",
					_referenceToLog, callbackData->_processedOutputTimestampMilliSecs.count(),
					_durationMilliSeconds ? *_durationMilliSeconds : static_cast<int64_t>(-1),
					callbackData->_progressPercent ? *callbackData->_progressPercent : -1.0
					);
			}
			else
			{
				if (ffmpegLine.empty())
					LOG_INFO("ffmpegLineCallback, line is empty"
						"{}", _referenceToLog);
				else
					LOG_WARN("ffmpegLineCallback, line not managed"
						"{}"
						", ffmpegLine: {}", _referenceToLog, ffmpegLine);

				if (callbackData->_ffmpegOutputLogFile)
				{
					const  std::string dateInfo = std::format("[{}] ",
						Datetime::nowLocalTime("%Y-%m-%d %H:%M:%S.", true));
					callbackData->_ffmpegOutputLogFile.write(dateInfo.data(), dateInfo.size());
					callbackData->_ffmpegOutputLogFile.write(ffmpegLine.data(), ffmpegLine.size());
					callbackData->_ffmpegOutputLogFile.write("\n", 1);
					callbackData->_ffmpegOutputLogFile.flush();
				}
			}
		}
	}
	catch (std::exception& e)
	{
		LOG_ERROR(
			"ffmpegLineCallback, exception"
			"{}"
			", ffmpegLine: {}"
			", exception: {}",
			_referenceToLog, ffmpegLine, e.what()
		);
	}
}

 std::string FFMpegEngine::Output::toSingleLine() const
{
	 std::vector< std::string> args;
	buildArgs(args);

	return FFMpegEngine::toSingleLine(args);
}

// ---------------- builder methods ----------------

FFMpegEngine& FFMpegEngine::addGlobalArg(const  std::string_view& arg) {
	 std::string_view trimmed = StringUtils::trim(arg);
	if (!trimmed.empty())
	    _globalArgs.emplace_back(trimmed);
    return *this;
}

FFMpegEngine& FFMpegEngine::addGlobalArgs(const  std::string& parameters)
{
	for (auto&& tok :
		parameters | std::views::split(' ') | std::views::filter([](auto &&rng){ return !std::ranges::empty(rng); }))
		_globalArgs.emplace_back(tok.begin(), tok.end());
    return *this;
}

FFMpegEngine::Input& FFMpegEngine::addInput(const  std::string_view source) {
	_inputs.emplace_back(source);
	return _inputs.back();
}

FFMpegEngine::Input& FFMpegEngine::addInput() {
	_inputs.emplace_back();
    return _inputs.back();
}

FFMpegEngine::Output& FFMpegEngine::addOutput(const  std::string_view path) {
	_outputs.emplace_back(path);
	return _outputs.back();
}

FFMpegEngine::Output& FFMpegEngine::addOutput() {
    _outputs.emplace_back();
    return _outputs.back();
}

FFMpegEngine& FFMpegEngine::addFilterComplex(const  std::string_view& fc) {
	if (!StringUtils::trim(fc).empty())
	    _filterComplex.emplace_back(StringUtils::trim(fc));
    return *this;
}

FFMpegEngine::Input& FFMpegEngine::addSrtInput(const  std::string_view& target,  std::optional<int> latencyMilliSeconds) {
    auto& in = addInput(std::format("srt://{}", target));
    if (latencyMilliSeconds)
    	in.addArg( std::string("-timeout ") + std::to_string(*latencyMilliSeconds));
    return in;
}

FFMpegEngine::Input& FFMpegEngine::addUdpInput(const  std::string_view& target,  std::optional<int> listenTimeoutMilliSeconds) {
	if (listenTimeoutMilliSeconds)
		return addInput(std::format("udp://{}?timeout=", target, *listenTimeoutMilliSeconds * 1000));
	return addInput(std::format("udp://{}", target));
}

FFMpegEngine::Input& FFMpegEngine::addRtmpInput(const  std::string_view& target) {
    return addInput(std::format("rtmp://{}", target));
}

FFMpegEngine::Input& FFMpegEngine::addPipeInput(const  std::string_view& spec) {
    return addInput(spec);
}

FFMpegEngine& FFMpegEngine::enableNvenc() {
    _hwAccel = "nvenc";
    return *this;
}

FFMpegEngine& FFMpegEngine::enableVaapi(const  std::string_view& device) {
    _hwAccel = "vaapi";
    _vaapiDevice =  std::string(device);
    return *this;
}

FFMpegEngine& FFMpegEngine::enableVideoToolbox() {
    _hwAccel = "videotoolbox";
    return *this;
}

// Prepare VAAPI upload filter and ensure device arg is present
FFMpegEngine& FFMpegEngine::vaapiPrepareUpload() {
    // Add a default hwupload filter for use in filter_complex consumers
    // Caller should add [in] format/ hwupload and map the output to encoder
    if (!_vaapiDevice)
    	_vaapiDevice = "/dev/dri/renderD128";
    // It's user's job to craft proper filter_complex, but we add a helper global arg
	addGlobalArgs(std::format("-vaapi_device {}", *_vaapiDevice));
    return *this;
}

FFMpegEngine& FFMpegEngine::addWatermark(Output& out,  std::string_view overlayLabel,  std::string_view pos) {
    out.addVideoFilter( std::string(overlayLabel) + " overlay=" +  std::string(pos));
    return *this;
}

void FFMpegEngine::setDurationMilliSeconds(const int64_t durationMilliSeconds) {
    _durationMilliSeconds = durationMilliSeconds;
}

// ---------------- build args (vector) ----------------

 std::string FFMpegEngine::toSingleLine( std::vector< std::string>& args)
{
	std::ostringstream ffmpegArgumentListStream;

	// if (!ffmpegArgumentList.empty())
	// 	copy(ffmpegArgumentList.begin(), ffmpegArgumentList.end(), ostream_iterator<string>(ffmpegArgumentListStream, " "));

	for (size_t index = 0; index < args.size(); ++index)
	{
		if (index)
			ffmpegArgumentListStream << " ";
		// simple quoting for visualization
		if (args[index].find(' ') !=  std::string::npos)
			ffmpegArgumentListStream << "\"" << args[index] << "\"";
		else
			ffmpegArgumentListStream << args[index];
	}

	return ffmpegArgumentListStream.str();
}

 std::vector< std::string> FFMpegEngine::buildArgs(bool useProgressPipe) const
{
     std::vector< std::string> args;

	args.emplace_back("ffmpeg");

    for (auto& g : _globalArgs)
    	args.emplace_back(g);

    // inputs
    for (auto& input : _inputs)
    	input.buildArgs(args);

    if (!_filterComplex.empty())
    {
        args.emplace_back("-filter_complex");
        // join
         std::string fc;
        for (size_t i = 0; i < _filterComplex.size(); ++i)
        {
            fc += _filterComplex[i];
            if (i + 1 < _filterComplex.size())
            	fc += ";";
        }
        args.emplace_back(fc);
    }

    // outputs
    for (auto& output : _outputs)
    	output.buildArgs(args);

    if (useProgressPipe)
    {
        args.emplace_back("-nostats");
        args.emplace_back("-progress");
        args.emplace_back("pipe:1");
    }

    return args;
}

 std::string FFMpegEngine::build(bool useProgressPipe) const
{
    auto args = buildArgs(useProgressPipe);

	return toSingleLine(args);
}

// ----------------- formatter per mostrare i comandi -----------------

 std::string FFMpegEngine::toPrettyString(const int indentSpaces) const {
	std::ostringstream oss;
	const std::string indent(indentSpaces, ' ');

	// --- INPUTS ---
	oss << "Inputs:\n";
	for (const auto &inp : _inputs) {
		std::string line;
		for (auto &opt : inp._args)
			line += opt + " ";
		line += std::format("-i {}", inp._source);
		oss << indent << line << "\n";
	}
	oss << "\n";

	// --- FILTERS ---
	if (!_filterComplex.empty()) {
		oss << "Filters:\n";
		std::vector<std::string> filterLines;
		for (const auto& f : _filterComplex)
				oss << indent << f << "\n";
		oss << "\n";
	}

	// --- OUTPUT ---
	for (const auto& out : _outputs)
	{
		oss << "Output: " << out._path << "\n";
		oss << indent << "Video codec: " << *out._videoCodec << "\n";
		oss << indent << "Audio codec: " << *out._audioCodec << "\n";
		for (const auto& opt : out._extraArgs)
			oss << indent << opt << "\n";
	}
	oss << "\n";

	return oss.str();
}

/// Formato singola linea, come vero comando ffmpeg
std::string FFMpegEngine::toSingleLine(bool useProgressPipe) const {

	return build(useProgressPipe);
}

void FFMpegEngine::reset()
{
	_inputs.clear();
	_outputs.clear();
	_filterComplex.clear();
	_globalArgs.clear();
	_hwAccel =  std::nullopt;
	_vaapiDevice =  std::nullopt;
	_durationMilliSeconds =  std::nullopt;
	_internalCallbackData->reset();
}