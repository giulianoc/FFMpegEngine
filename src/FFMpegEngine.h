
#pragma once

#include "StringUtils.h"
#include "ProcessUtility.h"

#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <vector>
#include <fstream>
#include <regex>
#include <shared_mutex>

#include "nlohmann/json.hpp"

class FFMpegEngine {
private:
	void ffmpegLineCallback(const std::string_view& ffmpegLine);
public:
	class CallbackData {
	public:
		CallbackData() = default;

		std::shared_ptr<CallbackData> clone()
		{
			std::shared_lock locker(_callbackDataMutex);

			auto clonedData = std::make_shared<CallbackData>();

			clonedData->_finished = _finished;

			clonedData->_startTime = _startTime;
			clonedData->_endTime = _endTime;
			clonedData->_outputFfmpegPathFileName = _outputFfmpegPathFileName;
			clonedData->_processedFrames = _processedFrames;
			clonedData->_framePerSeconds = _framePerSeconds;
			clonedData->_processedOutputTimestampMilliSecs = _processedOutputTimestampMilliSecs;
			clonedData->_speed = _speed;
			clonedData->_dropFrames = _dropFrames;
			clonedData->_dupFrames = _dupFrames;
			clonedData->_stream_0_0_q = _stream_0_0_q;
			clonedData->_stream_1_0_q = _stream_1_0_q;
			clonedData->_processedSizeKBps = _processedSizeKBps;
			clonedData->_bitRateKbps = _bitRateKbps;
			clonedData->_progressPercent = _progressPercent;
			clonedData->_avgBitRateKbps = _avgBitRateKbps;
			clonedData->_errorMessages = _errorMessages;

			clonedData->_urlForbidden = _urlForbidden;
			clonedData->_urlNotFound = _urlNotFound;
			clonedData->_nonMonotonousDts = _nonMonotonousDts;
			clonedData->_tlsError = _tlsError;
			clonedData->_openResourceError = _openResourceError;
			clonedData->_segmentFailedTooManyTimes = _segmentFailedTooManyTimes;
			clonedData->_timestampDiscontinuityCount = _timestampDiscontinuityCount;
			clonedData->_discontinuities = _discontinuities;
			clonedData->_ioEndOfFile = _ioEndOfFile;

			clonedData->_signal = _signal;

			return clonedData;
		}

		void setOutputFfmpegPathFileName(const std::string &outputFfmpegPathFileName)
		{
			std::unique_lock locker(_callbackDataMutex);
			_outputFfmpegPathFileName = outputFfmpegPathFileName;
		}

		static constexpr int32_t maxErrorsStored = 50;
		void pushErrorMessage(const std::string& errorMessage)
		{
			std::unique_lock locker(_callbackDataMutex);
			if (_errorMessages.size() >= maxErrorsStored)
				_errorMessages.pop();
			_errorMessages.push(errorMessage);
			const std::string lowerErrorMessage = StringUtils::lowerCase(errorMessage);
			if (!_urlForbidden && lowerErrorMessage.starts_with("403 forbidden"))
				_urlForbidden = true;
			if (!_urlNotFound && lowerErrorMessage.starts_with("404 not found"))
				_urlNotFound = true;
			if (!_nonMonotonousDts && lowerErrorMessage.find("non-monotonous dts in output stream") != std::string::npos)
				_nonMonotonousDts = true;
			if (!_tlsError && lowerErrorMessage.find("tlsv1 alert internal error") != std::string::npos)
				_tlsError = true;
			if (!_openResourceError && lowerErrorMessage.find("unable to open resource") != std::string::npos)
				_openResourceError = true;
			if (!_segmentFailedTooManyTimes && regex_match(lowerErrorMessage, std::regex("Segment .* failed too many times, skipping")))
				_segmentFailedTooManyTimes = true;
			// [vist#0:4/h264 @ 0x55562ce99140] timestamp discontinuity (stream id=3): -20048800, new offset= 82
			// [aist#0:0/aac @ 0x55562ced7800] timestamp discontinuity (stream id=0): 20048803, new offset= -20048721
			// [vist#0:4/h264 @ 0x55562ce99140] timestamp discontinuity (stream id=3): -20048800, new offset= 79
			// [aist#0:0/aac @ 0x55562ced7800] timestamp discontinuity (stream id=0): 20048804, new offset= -20048725
			// [vist#0:4/h264 @ 0x55562ce99140] timestamp discontinuity (stream id=3): -20048800, new offset= 75
			// [aist#0:0/aac @ 0x55562ced7800] timestamp discontinuity (stream id=0): 20048803, new offset= -20048728
			// [vist#0:4/h264 @ 0x55562ce99140] timestamp discontinuity (stream id=3): -20048811, new offset= 83
			// [aist#0:0/aac @ 0x55562ced7800] timestamp discontinuity (stream id=0): 20048803, new offset= -20048720
			// ...
			// Scenario:
			// Abbiamo tanti messaggi "timestamp discontinuity" (vedi sopra)
			// con i valori frame=, size=, time= corretti.
			// Indica che lo streaming sta andando avanti ma mancano tanti timestamp.
			// Questo messaggio di per se non richiede un restart, servirebbe il restart se
			// 1. ≥ N volte in M secondi
			// 2. dup crescente + discontinuity
			if (lowerErrorMessage.find("timestamp discontinuity") != std::string::npos)
			{
				_timestampDiscontinuityCount++;

				// finchè non abbiamo un nuovo discontinuity, _discontinuities non sarà aggiornato. Penso vada bene.
				const auto now = std::chrono::steady_clock::now();
				_discontinuities.push_back(now);
				{
					while (!_discontinuities.empty() && now - _discontinuities.front() > _timestampDiscontinuitiTimeWindow)
						_discontinuities.pop_front();
				}
			}
			// [tls @ 0x736e483b7e40] IO error: End of file
			// Scenario:
			// La connessione TLS verso il sorgente è caduta (es. URL firmati che scadono).
			// Il reconnect automatico di ffmpeg non rinegozia il token → restart necessario.
			// Restart se ≥ N occorrenze in M secondi (default: 3 in 10s).
			if (lowerErrorMessage.find("io error: end of file") != std::string::npos)
			{
				const auto now = std::chrono::steady_clock::now();
				_ioEndOfFile.push_back(now);
				while (!_ioEndOfFile.empty() && now - _ioEndOfFile.front() > _ioEndOfFileTimeWindow)
					_ioEndOfFile.pop_front();
			}
		}

		void reset()
		{
			std::unique_lock locker(_callbackDataMutex);

			if (_ffmpegOutputLogFile)
				_ffmpegOutputLogFile.close();

			_outputFfmpegPathFileName = "";
			_startTime = std::nullopt;
			_endTime = std::nullopt;
			_processedFrames = 0;
			_framePerSeconds = 0.0;
			_processedOutputTimestampMilliSecs = std::chrono::milliseconds(0);
			_speed = 0.0;
			_dropFrames = 0;
			_dupFrames = 0;
			_stream_0_0_q = 0.0;
			_stream_1_0_q = 0.0;
			_processedSizeKBps = 0;
			_bitRateKbps = 0.0;
			_progressPercent = std::nullopt;
			_avgBitRateKbps = 0.0;

			_urlForbidden = false;
			_urlNotFound = false;
			_nonMonotonousDts = false;
			_tlsError = false;
			_openResourceError = false;
			_segmentFailedTooManyTimes = false;
			_timestampDiscontinuityCount = 0;
			_discontinuities.clear();
			_ioEndOfFile.clear();

			_signal = std::nullopt;

			_finished = std::nullopt;

			while (!_errorMessages.empty())
				_errorMessages.pop();
		}

		nlohmann::json toJson(const bool errorMessagesToBeReset = false)
		{
			std::shared_lock locker(_callbackDataMutex);

			if (!_finished) // indica che Data non è stato usato
				return nullptr;
			nlohmann::json root;
			root["outputFfmpegPathFileName"] = _outputFfmpegPathFileName;
			root["processedFrames"] = _processedFrames;
			root["framePerSeconds"] = _framePerSeconds;
			root["processedOutputTimestampMilliSecs"] = _processedOutputTimestampMilliSecs.count();
			root["speed"] = _speed;
			root["dropFrames"] = _dropFrames;
			root["dupFrames"] = _dupFrames;
			root["stream_0_0_q"] = _stream_0_0_q;
			root["stream_1_0_q"] = _stream_1_0_q;
			root["processedSizeKBps"] = _processedSizeKBps;
			root["bitRateKbps"] = _bitRateKbps;
			root["avgBitRateKbps"] = _avgBitRateKbps;
			root["urlForbidden"] = _urlForbidden;
			root["urlNotFound"] = _urlNotFound;
			root["nonMonotonousDts"] = _nonMonotonousDts;
			root["timestampDiscontinuityCount"] = _timestampDiscontinuityCount;
			root[std::format("timestampDiscontinuityCount in {} seconds", _timestampDiscontinuitiTimeWindow)] = _discontinuities.size();
			root[std::format("ioEndOfFileCount in {} seconds", _ioEndOfFileTimeWindow)] = _ioEndOfFile.size();
			root["signal"] = this->_signal ? *this->_signal : -1;
			root["finished"] = *_finished;
			if (_startTime && _endTime)
				root["elapsed"] = std::chrono::duration_cast<std::chrono::milliseconds>(*_endTime - *_startTime).count();
			else
				root["elapsed"] = nullptr;
			if (_progressPercent)
				root["progressPercent"] = *_progressPercent;
			else
				root["progressPercent"] = nullptr;

			nlohmann::json errorMessagesRoot = nlohmann::json::array();
			if (errorMessagesToBeReset)
			{
				while (!_errorMessages.empty()) {
					errorMessagesRoot.push_back(_errorMessages.front());
					_errorMessages.pop();
				}
			}
			else
			{
				auto tmp = _errorMessages;   // copia della queue
				while (!tmp.empty()) {
					errorMessagesRoot.push_back(tmp.front());
					tmp.pop();
				}
			}
			root["errorMessages"] = errorMessagesRoot;
			return root;
		}

		std::optional<bool> getFinished()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _finished;
		}

		std::optional<double> getProgressPercent()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _progressPercent;
		}

		std::optional<int32_t> getSignal()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _signal;
		}

		bool getUrlForbidden()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _urlForbidden;
		}

		bool getUrlNotFound()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _urlNotFound;
		}

		bool getNonMonotonousDts()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _nonMonotonousDts;
		}

		int32_t getProcessedFrames()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _processedFrames;
		}

		double getFramePerSeconds()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _framePerSeconds;
		}

		double getSpeed()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _speed;
		}

		size_t getProcessedSizeKBps()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _processedSizeKBps;
		}

		std::chrono::milliseconds getProcessedOutputTimestampMilliSecs()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _processedOutputTimestampMilliSecs;
		}

		// restart se si verificano entrambi gli errori
		bool getTlsAndOpenResourceError()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _tlsError && _openResourceError;
		}

		bool getSegmentFailedTooManyTimes()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _segmentFailedTooManyTimes;
		}

		size_t getTimestampDiscontinuityCountInTimeWindow()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _discontinuities.size();
		}

		size_t getIoEndOfFileCountInTimeWindow()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _ioEndOfFile.size();
		}

		double getBitRateKbps()
		{
			std::shared_lock locker(_callbackDataMutex);
			return _bitRateKbps;
		}

	private:
		// lower case
		inline static const std::vector<std::string> errorPatterns = {
			"invalid data found",
			"error while decoding",
			"connection refused",
			"connection timed out",
			"network is unreachable",
			"protocol not found",
			"no such file",
			"broken pipe",
			"unknown encoder",
			"invalid argument",
			"unrecognized option",
			"timestamp discontinuity", // restart se ≥ N volte in M secondi. Es.: ≥ 5 volte in 30s oppure crescita continua per 60s
			"403 forbidden", // url forbidden
			"non-monotonous dts in output stream",
			"404 not found",	// url not found
			"ssl routines::tlsv1 alert internal error", // se si verifica questo error AND il prossimo, restart: combinazione di errori irreversibili
			"unable to open resource",
			"Segment .* failed too many times, skipping" // restart: errore irreversibile
		};

        friend void FFMpegEngine::ffmpegLineCallback(const std::string_view&);

		std::shared_mutex _callbackDataMutex;

		std::string _outputFfmpegPathFileName;
		std::ofstream _ffmpegOutputLogFile;

		std::optional<std::chrono::system_clock::time_point> _startTime{};
		std::optional<std::chrono::system_clock::time_point> _endTime{};

		int32_t _processedFrames{};
		double _framePerSeconds{};
		std::chrono::milliseconds _processedOutputTimestampMilliSecs{};
		double _speed{}; // Utile per capire se il server sta performando bene
		int32_t _dropFrames{};
		int32_t _dupFrames{};
		double _stream_0_0_q{};
		double _stream_1_0_q{};
		size_t _processedSizeKBps{};
		double _bitRateKbps{};
		std::optional<double> _progressPercent{}; // calcolato da noi se durata è stata settata
		double _avgBitRateKbps{};			 // calculated by us

		bool _urlForbidden{};
		bool _urlNotFound{};
		bool _nonMonotonousDts{};
		bool _tlsError{};
		bool _openResourceError{};
		bool _segmentFailedTooManyTimes{};

		uint32_t _timestampDiscontinuityCount{};
		// discontinuities: serve per capire se ≥ N volte in M secondi
		static constexpr auto _timestampDiscontinuitiTimeWindow = std::chrono::seconds(30);
		std::deque<std::chrono::steady_clock::time_point> _discontinuities; // steady_clock → immune a cambi ora / NTP

		static constexpr auto _ioEndOfFileTimeWindow = std::chrono::seconds(10);
		std::deque<std::chrono::steady_clock::time_point> _ioEndOfFile;

		std::optional<int32_t> _signal{};

		// nullopt se Data non è stato utilizzato
		// false se viene usato ma non è ancora terminato
		// true se viene usato ed è terminato (progress=end)
		std::optional<bool> _finished = std::nullopt;

		std::queue<std::string> _errorMessages;
	};

    class Input {
    	friend FFMpegEngine;

		std::string _source;
        std::vector<std::string> _args;
		int32_t _durationSeconds = -1;
    public:
    	Input() = default;
    	explicit Input(const std::string_view& source)
    	{
    		// se source è audio="CABLE Output (VB-Audio Virtual Cable)", dobbiamo rimuovere le virgolette per evitare problemi a ffmpeg
    		_source.clear();
    		_source.reserve(source.size());
    		std::ranges::copy_if(source, std::back_inserter(_source),
			[](char c){ return c != '"'; });
    	}
		Input& setSource(const std::string_view& source)
    	{
    		// se source è audio="CABLE Output (VB-Audio Virtual Cable)", dobbiamo rimuovere le virgolette per evitare problemi a ffmpeg
    		_source.clear();
    		_source.reserve(source.size());
    		std::ranges::copy_if(source, std::back_inserter(_source), [](char c){ return c != '"'; });
    		return *this;
    	}
		Input& setDurationSeconds(const int32_t durationSeconds) { _durationSeconds = durationSeconds; return *this; }
		Input& addArg(const std::string_view& parameter);
    	Input& addArgs(const std::string& parameters);
		void buildArgs(std::vector<std::string> &args) const;
		[[nodiscard]] std::string toSingleLine() const;
	};

    class Output {
    	friend FFMpegEngine;

    	std::string _path;

    	std::vector<std::string> _maps;
    	bool _copyAllTracks{};

        std::vector<std::string> _videoFilters;
        std::vector<std::string> _audioFilters;
        std::optional<std::string> _videoCodec;
        std::optional<std::string> _audioCodec;
        std::vector<std::string> _extraArgs;
    public:
        Output() = default;
    	explicit Output(const std::string_view& path) : _path(path) {}
		Output& setPath(const std::string_view& path) { _path = path; return *this; }
        Output& map(std::string_view m) { _maps.emplace_back(m); return *this; }
    	Output& setCopyAllTracks(const bool copyAllTracks) { _copyAllTracks = copyAllTracks; return *this; }
        Output& withVideoCodec(std::string_view c) { _videoCodec = std::string(c); return *this; }
        Output& withAudioCodec(std::string_view c) { _audioCodec = std::string(c); return *this; }
        Output& addVideoFilter(std::string_view f) { _videoFilters.emplace_back(f); return *this; }
    	size_t videoFilterSize() const { return _videoFilters.size(); }
        Output& addAudioFilter(std::string_view f) { _audioFilters.emplace_back(f); return *this; }
    	size_t audioFilterSize() const { return _audioFilters.size(); }
        Output& addArg(const std::string_view& parameter);
     	Output& addArgs(const std::string& parameters);
    	void buildArgs(std::vector<std::string>& args) const;
		[[nodiscard]] std::string toSingleLine() const;
   };

    FFMpegEngine()
    {
    	_internalCallbackData = std::make_shared<CallbackData>();
    };

    // builder
    FFMpegEngine& addGlobalArg(const std::string_view &arg);
	FFMpegEngine& addGlobalArgs(const std::string& parameters);
    Input& addInput(std::string_view source);
    Input& addInput();
    Output& addOutput(std::string_view path);
    Output& addOutput();
    FFMpegEngine& addFilterComplex(const std::string_view &fc);

    // convenience inputs
	Input& addUdpInput(const std::string_view& target, std::optional<int> listenTimeoutMilliSeconds = {});
    Input& addSrtInput(const std::string_view &target, std::optional<int> latencyMilliSeconds = {});
    Input& addRtmpInput(const std::string_view &target);
    Input& addPipeInput(const std::string_view &spec);

    // HW accel
    FFMpegEngine& enableNvenc();
    FFMpegEngine& enableVaapi(const std::string_view &device = "/dev/dri/renderD128");
    FFMpegEngine& enableVideoToolbox();

    // VAAPI convenience: prepare upload and choose codec names (adds filters/args as needed)
    // After calling this, for VAAPI outputs prefer videoCodec "h264_vaapi" or "hevc_vaapi"
    FFMpegEngine& vaapiPrepareUpload();

    // TODO: watermark
    FFMpegEngine& addWatermark(Output& out, std::string_view overlayLabel, std::string_view pos = "10:10");

    // duration for percent calculation (ms). If set, progress percent = out_time_ms / durationMilliSeconds
    void setDurationMilliSeconds(int64_t durationMilliSeconds);
	double getProgressPercent() const;

	// ---------------- Utility methods ----------------
	static std::string toSingleLine(std::vector<std::string> &args) ;

	// build command (not shell-quoted). useProgressPipe true adds -progress pipe:1
    [[nodiscard]] std::string build(bool useProgressPipe = false) const;
    [[nodiscard]] std::vector<std::string> buildArgs(bool useProgressPipe = false) const;

	void run(const std::string& ffmpegPath, ProcessUtility::ProcessId& processId,
		int &iReturnedStatus, const std::string& referenceToLog,
		const std::shared_ptr<CallbackData> &clientCallbackData = nullptr, const std::string& outputFfmpegPathFileName = "");

	[[nodiscard]] std::string toPrettyString(int indentSpaces = 2) const;
	[[nodiscard]] std::string toSingleLine(bool useProgressPipe = false) const;

	void reset();

private:
    std::vector<Input> _inputs;
    std::vector<Output> _outputs;
    std::vector<std::string> _filterComplex;
    std::vector<std::string> _globalArgs;
    std::optional<std::string> _hwAccel;
    std::optional<std::string> _vaapiDevice;

    std::optional<int64_t> _durationMilliSeconds;

	std::shared_ptr<CallbackData> _internalCallbackData;
	std::shared_ptr<CallbackData> _clientCallbackData;
	std::string _referenceToLog;
};