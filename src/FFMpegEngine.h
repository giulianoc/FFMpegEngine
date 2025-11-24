
#pragma once

#include "ProcessUtility.h"
#include <string>
#include <string_view>
#include <vector>
#include <queue>
#include <optional>
#include <functional>
#include <thread>

#include "nlohmann/json.hpp"

#include <fstream>
#include <shared_mutex>

using namespace std;

using json = nlohmann::json;
using ordered_json = nlohmann::ordered_json;
using namespace nlohmann::literals;

class FFMpegEngine {
private:
	void ffmpegLineCallback(const string_view& ffmpegLine);
public:
	class CallbackData {
	public:
		CallbackData() = default;

		shared_ptr<CallbackData> clone()
		{
			shared_lock locker(_callbackDataMutex);

			auto clonedData = make_shared<CallbackData>();

			clonedData->_outputFfmpegPathFileName = _outputFfmpegPathFileName;
			clonedData->_processedFrames = _processedFrames;
			clonedData->_framePerSeconds = _framePerSeconds;
			clonedData->_processedOutputTimestampMilliSecs = _processedOutputTimestampMilliSecs;
			clonedData->_speed = _speed;
			clonedData->_dropFrames = _dropFrames;
			clonedData->_dupFrames = _dupFrames;
			clonedData->_stream_0_0_q = _stream_0_0_q;
			clonedData->_stream_1_0_q = _stream_1_0_q;
			clonedData->_totalSizeKBps = _totalSizeKBps;
			clonedData->_bitRateKbps = _bitRateKbps;
			clonedData->_avgBitRateKbps = _avgBitRateKbps;
			clonedData->_finished = _finished;
			clonedData->_errorMessages = _errorMessages;

			clonedData->_urlForbidden = _urlForbidden;
			clonedData->_urlNotFound = _urlNotFound;

			clonedData->_signal = _signal;

			return clonedData;
		}

		void setOutputFfmpegPathFileName(const string &outputFfmpegPathFileName)
		{
			unique_lock locker(_callbackDataMutex);
			_outputFfmpegPathFileName = outputFfmpegPathFileName;
		}

		static constexpr int32_t maxErrorsStored = 50;
		void pushErrorMessage(const string& errorMessage)
		{
			unique_lock locker(_callbackDataMutex);
			if (_errorMessages.size() >= maxErrorsStored)
				_errorMessages.pop();
			_errorMessages.push(errorMessage);
			if (!_urlForbidden && errorMessage.starts_with("403 forbidden"))
				_urlForbidden = true;
			if (!_urlNotFound && errorMessage.starts_with("404 not found"))
				_urlNotFound = true;
		}

		void reset()
		{
			unique_lock locker(_callbackDataMutex);

			if (_ffmpegOutputLogFile)
				_ffmpegOutputLogFile.close();

			_outputFfmpegPathFileName = "";
			_processedFrames = 0;
			_framePerSeconds = 0.0;
			_processedOutputTimestampMilliSecs = chrono::milliseconds(0);
			_speed = 0.0;
			_dropFrames = 0;
			_dupFrames = 0;
			_stream_0_0_q = 0.0;
			_stream_1_0_q = 0.0;
			_totalSizeKBps = 0;
			_bitRateKbps = 0.0;
			_avgBitRateKbps = 0.0;

			_urlForbidden = false;
			_urlNotFound = false;

			_signal = nullopt;

			_finished = nullopt;

			while (!_errorMessages.empty())
				_errorMessages.pop();
		}

		json toJson()
		{
			shared_lock locker(_callbackDataMutex);

			if (!_finished) // indica che Data non è stato usato
				return nullptr;
			json root;
			root["outputFfmpegPathFileName"] = _outputFfmpegPathFileName;
			root["processedFrames"] = _processedFrames;
			root["framePerSeconds"] = _framePerSeconds;
			root["processedOutputTimestampMilliSecs"] = _processedOutputTimestampMilliSecs.count();
			root["speed"] = _speed;
			root["dropFrames"] = _dropFrames;
			root["dupFrames"] = _dupFrames;
			root["stream_0_0_q"] = _stream_0_0_q;
			root["stream_1_0_q"] = _stream_1_0_q;
			root["totalSizeKBps"] = _totalSizeKBps;
			root["bitRateKbps"] = _bitRateKbps;
			root["avgBitRateKbps"] = _avgBitRateKbps;
			root["urlForbidden"] = _urlForbidden;
			root["urlNotFound"] = _urlNotFound;
			root["signal"] = this->_signal ? *this->_signal : -1;
			root["finished"] = *_finished;

			json errorMessagesRoot = json::array();
			auto tmp = _errorMessages;   // copia della queue
			while (!tmp.empty()) {
				errorMessagesRoot.push_back(tmp.front());
				tmp.pop();
			}
			root["errorMessages"] = errorMessagesRoot;
			return root;
		}

		optional<bool> getFinished()
		{
			shared_lock locker(_callbackDataMutex);
			return _finished;
		}

		optional<int32_t> getSignal()
		{
			shared_lock locker(_callbackDataMutex);
			return _signal;
		}

		bool getUrlForbidden()
		{
			shared_lock locker(_callbackDataMutex);
			return _urlForbidden;
		}

		bool getUrlNotFound()
		{
			shared_lock locker(_callbackDataMutex);
			return _urlNotFound;
		}

	private:
		// lower case
		inline static const std::vector<string> errorPatterns = {
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
			"403 forbidden", // url forbidden
			"non-monotonous dts in output stream",
			"404 not found"	// url not found
		};

        friend void FFMpegEngine::ffmpegLineCallback(const string_view&);

		shared_mutex _callbackDataMutex;

		string _outputFfmpegPathFileName;
		ofstream _ffmpegOutputLogFile;

		int32_t _processedFrames{};
		double _framePerSeconds{};
		chrono::milliseconds _processedOutputTimestampMilliSecs{};
		double _speed{}; // Utile per capire se il server sta performando bene
		int32_t _dropFrames{};
		int32_t _dupFrames{};
		double _stream_0_0_q{};
		double _stream_1_0_q{};
		size_t _totalSizeKBps{};
		double _bitRateKbps{};
		double _avgBitRateKbps{};	// calculated by us

		bool _urlForbidden{};
		bool _urlNotFound{};

		optional<int32_t> _signal{};

		// nullopt se Data non è stato utilizzato, false se viene usato ma non è ancora terminato, true se viene usato ed è terminato (progress=end)
		optional<bool> _finished = nullopt;

		queue<string> _errorMessages;
	};

    class Input {
    	friend FFMpegEngine;

		string _source;
        vector<string> _args;
		int32_t _durationSeconds = -1;
    public:
    	Input() = default;
    	explicit Input(const string_view& source) : _source(source) {}
		Input& setSource(const string_view& source) { _source = source; return *this; }
		Input& setDurationSeconds(const int32_t durationSeconds) { _durationSeconds = durationSeconds; return *this; }
		Input& addArg(const string_view& parameter);
    	Input& addArgs(const string& parameters);
		void buildArgs(vector<string> &args) const;
		[[nodiscard]] string toSingleLine() const;
	};

    class Output {
    	friend FFMpegEngine;

    	string _path;
        vector<string> _maps;
        vector<string> _videoFilters;
        vector<string> _audioFilters;
        optional<string> _videoCodec;
        optional<string> _audioCodec;
        vector<string> _extraArgs;
    public:
        Output() = default;
    	explicit Output(const string_view& path) : _path(path) {}
		Output& setPath(const string_view& path) { _path = path; return *this; }
        Output& map(string_view m) { _maps.emplace_back(m); return *this; }
        Output& withVideoCodec(string_view c) { _videoCodec = string(c); return *this; }
        Output& withAudioCodec(string_view c) { _audioCodec = string(c); return *this; }
        Output& addVideoFilter(string_view f) { _videoFilters.emplace_back(f); return *this; }
    	size_t videoFilterSize() const { return _videoFilters.size(); }
        Output& addAudioFilter(string_view f) { _audioFilters.emplace_back(f); return *this; }
    	size_t audioFilterSize() const { return _audioFilters.size(); }
        Output& addArg(const string_view& parameter);
     	Output& addArgs(const string& parameters);
    	void buildArgs(vector<string>& args) const;
		[[nodiscard]] string toSingleLine() const;
   };

    FFMpegEngine()
    {
    	_internalCallbackData = make_shared<CallbackData>();
    };

    // builder
    FFMpegEngine& addGlobalArg(const string_view &a);
	FFMpegEngine& addGlobalArgs(const string& parameters);
    Input& addInput(string_view source);
    Input& addInput();
    Output& addOutput(string_view path);
    Output& addOutput();
    FFMpegEngine& addFilterComplex(const string_view &fc);

    // convenience inputs
	Input& addUdpInput(const string_view& target, optional<int> listenTimeoutMilliSeconds = {});
    Input& addSrtInput(const string_view &target, optional<int> latencyMilliSeconds = {});
    Input& addRtmpInput(const string_view &target);
    Input& addPipeInput(const string_view &spec);

    // HW accel
    FFMpegEngine& enableNvenc();
    FFMpegEngine& enableVaapi(const string_view &device = "/dev/dri/renderD128");
    FFMpegEngine& enableVideoToolbox();

    // VAAPI convenience: prepare upload and choose codec names (adds filters/args as needed)
    // After calling this, for VAAPI outputs prefer videoCodec "h264_vaapi" or "hevc_vaapi"
    FFMpegEngine& vaapiPrepareUpload();

    // TODO: watermark
    FFMpegEngine& addWatermark(Output& out, string_view overlayLabel, string_view pos = "10:10");

    // duration for percent calculation (ms). If set, progress percent = out_time_ms / durationMilliSeconds
    void setDurationMilliSeconds(int64_t durationMilliSeconds);
	static string toSingleLine(vector<string> &args) ;

	// build command (not shell-quoted). useProgressPipe true adds -progress pipe:1
    [[nodiscard]] string build(bool useProgressPipe = false) const;
    [[nodiscard]] vector<string> buildArgs(bool useProgressPipe = false) const;

	void run(const string& ffmpegPath, ProcessUtility::ProcessId& processId,
		int &iReturnedStatus, const string& referenceToLog,
		const shared_ptr<CallbackData> &clientCallbackData = nullptr, const string& outputFfmpegPathFileName = "");

	[[nodiscard]] string toPrettyString(int indentSpaces = 2) const;
	[[nodiscard]] string toSingleLine(bool useProgressPipe = false) const;

	void reset();

private:
    vector<Input> _inputs;
    vector<Output> _outputs;
    vector<string> _filterComplex;
    vector<string> _globalArgs;
    optional<string> _hwAccel;
    optional<string> _vaapiDevice;

    optional<int64_t> _durationMilliSeconds;

	shared_ptr<CallbackData> _internalCallbackData;
	shared_ptr<CallbackData> _clientCallbackData;
	string _referenceToLog;
};